using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Data;
using System.Drawing;
using System.IO;
using System.IO.Ports;
using System.Linq;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace IAP_OTA_Remote_Upgrade_UpperComputer
{
    public partial class Form1 : Form
    {
        public const UInt16 HEADER = 0xAA55;

        // 定义协议命令码
        public enum OtaCommand : ushort
        {
            CMD_PING = 0x0100,
            CMD_RESET = 0x0101,
            CMD_GET_VERSION = 0x0102,
            CMD_PARAM_READ = 0x0500,
            CMD_PARAM_WRITE = 0x0501,
            CMD_UPDATE_START = 0x0300,
            CMD_UPDATE_DATA = 0x0301,
            CMD_UPDATE_END = 0x0302,
            CMD_ACK = 0xFF00,
            CMD_NACK = 0xFF01
        }

        private const int UpgradeChunkSize = 240; // UPDATE_DATA: 240字节数据，对应整帧252字节(240+12)
        private const uint DefaultTarget = 1;   // 1=APP_AREA_A, 2=APP_AREA_B
        private const uint DefaultVersion = 1;

        private readonly SerialPort sp1 = new SerialPort();
        private readonly List<byte> _rxFrameBuffer = new List<byte>();
        private readonly object _rxLock = new object();
        private readonly AutoResetEvent _ackEvent = new AutoResetEvent(false);
        private readonly Decoder _utf8Decoder = Encoding.UTF8.GetDecoder();
        private readonly StringBuilder _utf8RxBuffer = new StringBuilder();
        private readonly object _utf8LogLock = new object();

        private volatile int _lastAckState = 0; // 1=ACK, -1=NACK, 0=未收到
        private bool _isUpgrading = false;

        public Form1()
        {
            InitializeComponent();
        }

        private void groupBox1_Enter(object sender, EventArgs e)
        {

        }

        private void openFileDialog1_FileOk(object sender, CancelEventArgs e)
        {

        }

        private void button4_Click(object sender, EventArgs e)
        {
            SendProtocolData(OtaCommand.CMD_PING);
        }

        /// <summary>
        /// 基于协议格式打包数据，并发送至串口
        /// </summary>
        private bool SendProtocolData(OtaCommand command, byte[] data = null)
        {
            if (!sp1.IsOpen)
            {
                MessageBox.Show("请先打开串口！");
                return false;
            }

            try
            {
                byte[] packet = BuildFrame(command, data);

                // 发送数据
                sp1.Write(packet, 0, packet.Length);

                // 发送日志：显示命令和长度（不显示HEX）
                int dataLen = data == null ? 0 : data.Length;
                AppendLog($"[TX] {command}, Length={dataLen}", Color.Blue);

                return true;
            }
            catch (Exception ex)
            {
                MessageBox.Show("发送数据失败: " + ex.Message);
                return false;
            }
        }

        private byte[] BuildFrame(OtaCommand command, byte[] data)
        {
            List<byte> frame = new List<byte>();

            // 1. Header (2 bytes) - 0xAA55 (线上发送: 55 AA)
            frame.AddRange(BitConverter.GetBytes((ushort)HEADER));

            // 2. Command (2 bytes)
            frame.AddRange(BitConverter.GetBytes((ushort)command));

            // 3. Length (2 bytes)
            ushort dataLen = (ushort)(data == null ? 0 : data.Length);
            frame.AddRange(BitConverter.GetBytes(dataLen));

            // 4. Data (N bytes)
            if (dataLen > 0)
            {
                frame.AddRange(data);
            }

            // 5. Reserved (4 bytes)
            frame.AddRange(new byte[] { 0x00, 0x00, 0x00, 0x00 });
            
            // 6. CRC16 (2 bytes)
            ushort crc = CalculateCRC16(frame.ToArray());
            frame.AddRange(BitConverter.GetBytes(crc));

            return frame.ToArray();
        }

        /// <summary>
        /// 计算 CRC16-MODBUS 校验码
        /// </summary>
        private ushort CalculateCRC16(byte[] data)
        {
            ushort crc = 0xFFFF;
            for (int i = 0; i < data.Length; i++)
            {
                crc ^= data[i];
                for (int j = 0; j < 8; j++)
                {
                    if ((crc & 0x0001) != 0)
                    {
                        crc >>= 1;
                        crc ^= 0xA001;
                    }
                    else
                    {
                        crc >>= 1;
                    }
                }
            }
            return crc;
        }

        private uint CalculateCrc32Stm(byte[] alignedData)
        {
            uint crc = 0xFFFFFFFF;

            for (int i = 0; i < alignedData.Length; i += 4)
            {
                uint word = (uint)(alignedData[i]
                    | (alignedData[i + 1] << 8)
                    | (alignedData[i + 2] << 16)
                    | (alignedData[i + 3] << 24));

                crc ^= word;
                for (int bit = 0; bit < 32; bit++)
                {
                    if ((crc & 0x80000000) != 0)
                    {
                        crc = (crc << 1) ^ 0x04C11DB7;
                    }
                    else
                    {
                        crc <<= 1;
                    }
                }
            }

            return crc;
        }

        private byte[] PadTo4Bytes(byte[] raw)
        {
            int rem = raw.Length % 4;
            if (rem == 0)
            {
                return raw;
            }

            int pad = 4 - rem;
            byte[] aligned = new byte[raw.Length + pad];
            Buffer.BlockCopy(raw, 0, aligned, 0, raw.Length);
            for (int i = raw.Length; i < aligned.Length; i++)
            {
                aligned[i] = 0xFF;
            }

            return aligned;
        }

        private void ResetAckState()
        {
            _lastAckState = 0;
            while (_ackEvent.WaitOne(0)) { }
        }

        private bool SendAndWaitAck(OtaCommand cmd, byte[] data, int timeoutMs)
        {
            ResetAckState();

            if (!SendProtocolData(cmd, data))
            {
                return false;
            }

            if (!_ackEvent.WaitOne(timeoutMs))
            {
                AppendLog($"[ERR] 等待 {cmd} ACK 超时({timeoutMs}ms)", Color.Red);
                return false;
            }

            if (_lastAckState != 1)
            {
                AppendLog($"[ERR] {cmd} 收到 NACK", Color.Red);
                return false;
            }

            return true;
        }

        private bool RunUpgrade(string filePath, uint targetArea)
        {
            try
            {
                byte[] raw = File.ReadAllBytes(filePath);
                byte[] aligned = PadTo4Bytes(raw);
                uint crc32 = CalculateCrc32Stm(aligned);

                AppendLog($"[UPG] 原始大小: {raw.Length} 字节", Color.Black);
                AppendLog($"[UPG] 对齐大小: {aligned.Length} 字节", Color.Black);
                AppendLog($"[UPG] CRC32: 0x{crc32:X8}", Color.Black);

                this.Invoke(new Action(() =>
                {
                    label2.Text = $"对齐大小: {aligned.Length} B";
                    label3.Text = $"CRC32: 0x{crc32:X8}";
                    progressBar1.Minimum = 0;
                    progressBar1.Maximum = aligned.Length;
                    progressBar1.Value = 0;
                }));

                // Start: target/version/size/crc32 (16字节, 小端)
                List<byte> startData = new List<byte>(16);
                startData.AddRange(BitConverter.GetBytes(targetArea));
                startData.AddRange(BitConverter.GetBytes(DefaultVersion));
                startData.AddRange(BitConverter.GetBytes((uint)aligned.Length));
                startData.AddRange(BitConverter.GetBytes(crc32));

                if (!SendAndWaitAck(OtaCommand.CMD_UPDATE_START, startData.ToArray(), 8000))
                {
                    return false;
                }

                int sent = 0;
                while (sent < aligned.Length)
                {
                    int len = Math.Min(UpgradeChunkSize, aligned.Length - sent);
                    byte[] chunk = new byte[len];
                    Buffer.BlockCopy(aligned, sent, chunk, 0, len);

                    if (!SendAndWaitAck(OtaCommand.CMD_UPDATE_DATA, chunk, 2000))
                    {
                        return false;
                    }

                    sent += len;
                    this.Invoke(new Action(() =>
                    {
                        progressBar1.Value = sent;
                    }));
                }

                if (!SendAndWaitAck(OtaCommand.CMD_UPDATE_END, null, 5000))
                {
                    return false;
                }

                AppendLog("[UPG] 升级完成，设备已通过校验", Color.DarkGreen);
                return true;
            }
            catch (Exception ex)
            {
                AppendLog("[ERR] 升级异常: " + ex.Message, Color.Red);
                return false;
            }
        }

        private void AppendLog(string text, Color color)
        {
            if (this.InvokeRequired)
            {
                this.BeginInvoke(new Action(() => AppendLog(text, color)));
                return;
            }

            richTextBox1.SelectionColor = color;
            richTextBox1.AppendText(text + Environment.NewLine);
            richTextBox1.ScrollToCaret();
        }

        private void textBox3_TextChanged(object sender, EventArgs e)
        {

        }

        private void textBox2_TextChanged(object sender, EventArgs e)
        {

        }

        private void button3_Click(object sender, EventArgs e)
        {
            // 设置文件选择对话框
            openFileDialog1.Title = "请选择固件升级包";
            openFileDialog1.Filter = "Bin文件 (*.bin)|*.bin|所有文件 (*.*)|*.*";
            openFileDialog1.FileName = ""; // 清空默认文本

            // 如果用户点击了“确定”
            if (openFileDialog1.ShowDialog() == DialogResult.OK)
            {
                // 获取文件路径并显示在文本框
                string filePath = openFileDialog1.FileName;
                textBox3.Text = filePath;

                // 获取文件大小
                FileInfo fileInfo = new FileInfo(filePath);
                long fileSizeBytes = fileInfo.Length;

                // 在 label 处打印文件大小，格式化为字节和KB
                label1.Text = $"文件大小：{fileSizeBytes} 字节 ({fileSizeBytes / 1024.0:F2} KB)";

                // 选择文件后立即计算对齐大小和CRC32
                byte[] raw = File.ReadAllBytes(filePath);
                byte[] aligned = PadTo4Bytes(raw);
                uint crc32 = CalculateCrc32Stm(aligned);
                label2.Text = $"对齐大小: {aligned.Length} B";
                label3.Text = $"CRC32: 0x{crc32:X8}";
            }
        }

        private void comboBox1_SelectedIndexChanged(object sender, EventArgs e)
        {

        }

        private void button2_Click(object sender, EventArgs e)
        {
            try
            {
                if (!sp1.IsOpen)
                {
                    sp1.PortName = comboBox2.Text;
                    sp1.BaudRate = int.Parse(comboBox1.Text);
                    sp1.DataBits = 8;
                    sp1.StopBits = StopBits.One;
                    sp1.Parity = Parity.None;
                    sp1.Handshake = Handshake.None;
                    sp1.Encoding = Encoding.UTF8;

                    sp1.Open();
                    button2.Text = "关闭串口";
                }
                else
                {
                    sp1.Close();
                    button2.Text = "打开串口";
                }
            }
            catch (Exception ex)
            {
                MessageBox.Show("串口操作失败： " + ex.Message);
            }
        }

        private void richTextBox1_TextChanged(object sender, EventArgs e)
        {

        }

        private void comboBox2_SelectedIndexChanged(object sender, EventArgs e)
        {

        }

        private void Form1_Load(object sender, EventArgs e)
        {
            comboBox1.Items.AddRange(new string[]
            {
               "115200",
               "57600",
               "38400",
               "19200",
               "9600"
            });

            // 默认选中 115200 (注意：115200在第0位)
            comboBox1.SelectedIndex = 0;

            comboBox2.Items.AddRange(SerialPort.GetPortNames());

            // 绑定非升级指令到 comboBox3
            comboBox3.Items.AddRange(new string[]
            {
                "链路测试/心跳 (CMD_PING)",
                "请求设备软复位 (CMD_RESET)",
                "获取设备版本信息 (CMD_GET_VERSION)",
                "读取设备Flash参数区摘要 (CMD_PARAM_READ)"
            });
            comboBox3.SelectedIndex = 0;

            // 绑定升级目标分区到 comboBox4
            comboBox4.Items.AddRange(new string[]
            {
                "APP_A",
                "APP_B"
            });
            comboBox4.SelectedIndex = 0;

            label1.Text = "文件大小：-";
            label2.Text = "对齐大小：-";
            label3.Text = "CRC32：-";

            // 注册串口接收事件
            sp1.DataReceived += Sp1_DataReceived;
        }

        // 串口接收事件处理函数
        private void Sp1_DataReceived(object sender, SerialDataReceivedEventArgs e)
        {
            int bytesToRead = sp1.BytesToRead;
            if (bytesToRead <= 0)
            {
                return;
            }

            byte[] buffer = new byte[bytesToRead];
            sp1.Read(buffer, 0, bytesToRead);

            // UTF-8日志（按行输出，避免被拆段）
            AppendUtf8LinesFromBytes(buffer);

            // 追加到接收缓存并解析完整帧
            lock (_rxLock)
            {
                _rxFrameBuffer.AddRange(buffer);
                ParseReceivedFrames();
            }
        }

        private void AppendUtf8LinesFromBytes(byte[] data)
        {
            lock (_utf8LogLock)
            {
                char[] chars = new char[Encoding.UTF8.GetMaxCharCount(data.Length)];
                int count = _utf8Decoder.GetChars(data, 0, data.Length, chars, 0, false);
                if (count <= 0)
                {
                    return;
                }

                _utf8RxBuffer.Append(chars, 0, count);

                int start = 0;
                for (int i = 0; i < _utf8RxBuffer.Length; i++)
                {
                    if (_utf8RxBuffer[i] == '\n')
                    {
                        string line = _utf8RxBuffer.ToString(start, i - start).TrimEnd('\r');
                        if (!string.IsNullOrWhiteSpace(line))
                        {
                            AppendLog("[RX-UTF8] " + line, Color.DarkCyan);
                        }
                        start = i + 1;
                    }
                }

                if (start > 0)
                {
                    _utf8RxBuffer.Remove(0, start);
                }

                // 对于没有换行但累计较长的内容，也输出一次，避免看起来无日志
                if (_utf8RxBuffer.Length >= 120)
                {
                    string tail = _utf8RxBuffer.ToString().Trim();
                    if (!string.IsNullOrWhiteSpace(tail))
                    {
                        AppendLog("[RX-UTF8] " + tail, Color.DarkCyan);
                    }
                    _utf8RxBuffer.Clear();
                }
            }
        }

        private void ParseReceivedFrames()
        {
            while (true)
            {
                if (_rxFrameBuffer.Count < 12)
                {
                    return;
                }

                int headerIndex = -1;
                for (int i = 0; i <= _rxFrameBuffer.Count - 2; i++)
                {
                    if (_rxFrameBuffer[i] == 0x55 && _rxFrameBuffer[i + 1] == 0xAA)
                    {
                        headerIndex = i;
                        break;
                    }
                }

                if (headerIndex < 0)
                {
                    _rxFrameBuffer.Clear();
                    return;
                }

                if (headerIndex > 0)
                {
                    _rxFrameBuffer.RemoveRange(0, headerIndex);
                    if (_rxFrameBuffer.Count < 12)
                    {
                        return;
                    }
                }

                ushort len = (ushort)(_rxFrameBuffer[4] | (_rxFrameBuffer[5] << 8));
                if (len > 256)
                {
                    _rxFrameBuffer.RemoveAt(0);
                    continue;
                }

                int frameLen = 12 + len;
                if (_rxFrameBuffer.Count < frameLen)
                {
                    return;
                }

                byte[] frame = _rxFrameBuffer.GetRange(0, frameLen).ToArray();
                _rxFrameBuffer.RemoveRange(0, frameLen);

                ushort recvCrc = BitConverter.ToUInt16(frame, frameLen - 2);
                ushort calcCrc = CalculateCRC16(frame.Take(frameLen - 2).ToArray());
                if (recvCrc != calcCrc)
                {
                    AppendLog("[ERR] RX帧CRC错误", Color.Red);
                    continue;
                }

                ushort cmd = BitConverter.ToUInt16(frame, 2);
                if (cmd == (ushort)OtaCommand.CMD_ACK)
                {
                    AppendLog("[协议] 收到 ACK（成功应答）", Color.DarkGreen);
                    _lastAckState = 1;
                    _ackEvent.Set();
                }
                else if (cmd == (ushort)OtaCommand.CMD_NACK)
                {
                    AppendLog("[协议] 收到 NACK（失败应答）", Color.OrangeRed);
                    _lastAckState = -1;
                    _ackEvent.Set();
                }
            }
        }

        private string ToUtf8Display(byte[] data)
        {
            string text = Encoding.UTF8.GetString(data);
            StringBuilder sb = new StringBuilder(text.Length);

            for (int i = 0; i < text.Length; i++)
            {
                char c = text[i];
                if (c == '\r' || c == '\n' || c == '\t' || !char.IsControl(c))
                {
                    sb.Append(c);
                }
            }

            return sb.ToString().Trim();
        }

        private void button7_Click(object sender, EventArgs e)
        {

        }

        private void button6_Click(object sender, EventArgs e)
        {
            richTextBox1.Clear();
        }

        private void button7_Click_1(object sender, EventArgs e)
        {

        }

        private void label1_Click(object sender, EventArgs e)
        {

        }

        private void groupBox4_Enter(object sender, EventArgs e)
        {

        }

        private void button7_Click_2(object sender, EventArgs e)
        {

        }

        private void button7_Click_3(object sender, EventArgs e)
        {
            if (comboBox3.SelectedIndex == 0)
            {
                SendProtocolData(OtaCommand.CMD_PING);
            }
            else if (comboBox3.SelectedIndex == 1)
            {
                SendProtocolData(OtaCommand.CMD_RESET);
            }
            else if (comboBox3.SelectedIndex == 2)
            {
                SendProtocolData(OtaCommand.CMD_GET_VERSION);
            }
            else if (comboBox3.SelectedIndex == 3)
            {
                SendProtocolData(OtaCommand.CMD_PARAM_READ);
            }
        }

        private async void button5_Click(object sender, EventArgs e)
        {
            if (_isUpgrading)
            {
                MessageBox.Show("正在升级中，请勿重复点击。");
                return;
            }

            if (!sp1.IsOpen)
            {
                MessageBox.Show("请先打开串口。");
                return;
            }

            if (string.IsNullOrWhiteSpace(textBox3.Text) || !File.Exists(textBox3.Text))
            {
                MessageBox.Show("请先选择有效的 app.bin 文件。");
                return;
            }

            uint targetArea = GetSelectedTargetArea();

            _isUpgrading = true;
            button5.Enabled = false;

            bool ok = await Task.Run(() => RunUpgrade(textBox3.Text, targetArea));

            _isUpgrading = false;
            button5.Enabled = true;

            MessageBox.Show(ok ? "升级成功" : "升级失败，请查看日志");
       
        }

        private void comboBox3_SelectedIndexChanged(object sender, EventArgs e)
        {

        }

        private void label2_Click(object sender, EventArgs e)
        {

        }

        private void progressBar1_Click(object sender, EventArgs e)
        {

        }

        private void comboBox4_SelectedIndexChanged(object sender, EventArgs e)
        {

        }

        private uint GetSelectedTargetArea()
        {
            if (comboBox4.SelectedIndex == 1)
            {
                return 2; // APP_B
            }

            return DefaultTarget; // APP_A
        }
    }
}
