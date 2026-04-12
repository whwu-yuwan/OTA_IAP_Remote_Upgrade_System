using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Data;
using System.Drawing;
using System.IO;
using System.IO.Ports;
using System.Linq;
using System.Diagnostics;
using System.Net.Sockets;
using System.Net;
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
            CMD_UPDATE_ABORT = 0x0303,
            CMD_ACK = 0xFF00,
            CMD_NACK = 0xFF01,
            CMD_BUSY = 0xFF02
        }

        private const int UpgradeChunkSize = 240; // UPDATE_DATA: 240字节数据，对应整帧252字节(240+12)
        private const uint DefaultTarget = 1;   // 1=APP_AREA_A, 2=APP_AREA_B
        private const uint DefaultVersion = 1;
        private const int StartEndTimeoutMs = 5000;
        private const int DataTimeoutMs = 2000;
        private const int PacketRetryMax = 3;
        private static readonly int[] PacketBackoffMs = new[] { 300, 600, 1000 };
        private const int ReconnectRetryMax = 3;
        private const int ReconnectIntervalMs = 1000;
        private const int ReconnectWindowMs = 30000;

        private readonly SerialPort sp1 = new SerialPort();
        private TcpClient _tcpClient;
        private TcpListener _tcpListener;
        private readonly List<byte> _rxFrameBuffer = new List<byte>();
        private readonly object _rxLock = new object();
        private readonly Decoder _utf8Decoder = Encoding.UTF8.GetDecoder();
        private readonly StringBuilder _utf8RxBuffer = new StringBuilder();
        private readonly object _utf8LogLock = new object();
        private readonly SemaphoreSlim _singleFlightLock = new SemaphoreSlim(1, 1);
        private readonly AutoResetEvent _responseEvent = new AutoResetEvent(false);
        private readonly object _sessionLock = new object();
        private readonly object _transportLock = new object();
        private readonly object _upgradeSessionLock = new object();

        private bool _isUpgrading = false;
        private bool _abortRequested = false;
        private PendingCommandSession _pendingSession;
        private NetworkStream _tcpStream;
        private CancellationTokenSource _tcpReceiveCts;
        private Task _tcpReceiveTask;
        private CancellationTokenSource _tcpAcceptCts;
        private Task _tcpAcceptTask;
        private TransportKind _activeTransport = TransportKind.None;
        private TransportKind _upgradeSessionOwner = TransportKind.None;
        private int _lastTcpModeIndex = 1;
        private bool _suppressTcpModeChangeEvent = false;

        private enum CommandResultKind
        {
            None = 0,
            Ack = 1,
            Nack = 2,
            Busy = 3,
            Timeout = 4,
            SendFailed = 5,
            Aborted = 6,
            Echo = 7
        }

        private enum TransportKind
        {
            None = 0,
            Serial = 1,
            Tcp = 2
        }

        private enum UpgradeResultCode
        {
            OK,
            TIMEOUT,
            NACK,
            BUSY,
            CRC_ERR,
            LEN_ERR,
            DISCONNECTED,
            RETRY_EXHAUSTED
        }

        private sealed class PendingCommandSession
        {
            public OtaCommand RequestCmd;
            public bool ExpectEcho;
            public int TimeoutMs;
            public DateTime StartUtc;
            public CommandResultKind Result;
            public ushort ResponseCmd;
            public byte[] ResponseData;
        }

        private sealed class CommandSessionResult
        {
            public CommandResultKind Result;
            public ushort ResponseCmd;
            public byte[] ResponseData;
            public int BusyCount;
            public int RetryCount;
            public long ElapsedMs;

            public bool IsAckLikeSuccess
            {
                get
                {
                    return Result == CommandResultKind.Ack || Result == CommandResultKind.Echo;
                }
            }
        }

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
            Task.Run(() =>
            {
                var result = SendCommandInSingleFlight(OtaCommand.CMD_PING, Encoding.ASCII.GetBytes("PING"), 2000, true, 0);
                HandleSessionResult(OtaCommand.CMD_PING, result);
            });
        }

        /// <summary>
        /// 基于协议格式打包数据，并发送到当前激活的传输通道（串口/TCP）
        /// </summary>
        private bool SendProtocolData(OtaCommand command, byte[] data = null)
        {
            if (_activeTransport == TransportKind.None)
            {
                AppendStructuredLog("NONE", command.ToString(), "-", 0, UpgradeResultCode.DISCONNECTED, "transport-not-open", Color.Red);
                return false;
            }

            try
            {
                byte[] packet = BuildFrame(command, data);

                if (_activeTransport == TransportKind.Serial)
                {
                    if (!sp1.IsOpen)
                    {
                        AppendStructuredLog("UART", command.ToString(), "-", 0, UpgradeResultCode.DISCONNECTED, "serial-not-open", Color.Red);
                        return false;
                    }

                    sp1.Write(packet, 0, packet.Length);
                }
                else if (_activeTransport == TransportKind.Tcp)
                {
                    lock (_transportLock)
                    {
                        if (_tcpStream == null || _tcpClient == null || !_tcpClient.Connected)
                        {
                            AppendStructuredLog("WiFi", command.ToString(), "-", 0, UpgradeResultCode.DISCONNECTED, "tcp-not-connected", Color.Red);
                            return false;
                        }

                        _tcpStream.Write(packet, 0, packet.Length);
                        _tcpStream.Flush();
                    }
                }
                else
                {
                    AppendStructuredLog(_activeTransport.ToString(), command.ToString(), "-", 0, UpgradeResultCode.DISCONNECTED, "unknown-transport", Color.Red);
                    return false;
                }

                // 发送日志：显示命令和长度（不显示HEX）
                int dataLen = data == null ? 0 : data.Length;
                AppendStructuredLog(_activeTransport.ToString(), command.ToString(), dataLen.ToString(), 0, UpgradeResultCode.OK, "tx", Color.Blue);

                return true;
            }
            catch (Exception ex)
            {
                AppendStructuredLog(_activeTransport.ToString(), command.ToString(), "-", 0, UpgradeResultCode.DISCONNECTED, ex.Message, Color.Red);
                return false;
            }
        }

        private void ProcessIncomingBytes(byte[] buffer)
        {
            if (buffer == null || buffer.Length == 0)
            {
                return;
            }

            // 升级期间屏蔽UTF-8日志解析，避免ASCII日志与二进制帧混流时干扰观察
            if (!_isUpgrading)
            {
                AppendUtf8LinesFromBytes(buffer);
            }

            // 追加到接收缓存并解析完整帧
            lock (_rxLock)
            {
                _rxFrameBuffer.AddRange(buffer);
                ParseReceivedFrames();
            }
        }

        private void StartTcpReceiveLoop()
        {
            StopTcpReceiveLoop();
            _tcpReceiveCts = new CancellationTokenSource();
            var token = _tcpReceiveCts.Token;
            _tcpReceiveTask = Task.Run(() => TcpReceiveLoop(token), token);
        }

        private void StopTcpReceiveLoop()
        {
            try
            {
                if (_tcpReceiveCts != null)
                {
                    _tcpReceiveCts.Cancel();
                }
            }
            catch { }
        }

        private void CloseTcpTransport()
        {
            try { StopTcpReceiveLoop(); } catch { }
            try { StopTcpAcceptLoop(); } catch { }

            lock (_transportLock)
            {
                try
                {
                    if (_tcpStream != null)
                    {
                        _tcpStream.Close();
                        _tcpStream = null;
                    }
                }
                catch { }

                try
                {
                    if (_tcpClient != null)
                    {
                        _tcpClient.Close();
                        _tcpClient = null;
                    }
                }
                catch { }

                try
                {
                    if (_tcpListener != null)
                    {
                        _tcpListener.Stop();
                        _tcpListener = null;
                    }
                }
                catch { }
            }

            _tcpStream = null;
            _tcpReceiveTask = null;
            _tcpReceiveCts = null;
            _tcpAcceptTask = null;
            _tcpAcceptCts = null;

            if (_activeTransport == TransportKind.Tcp)
            {
                _activeTransport = TransportKind.None;
            }
        }

        private void OpenTcpTransport(string host, int port)
        {
            if (_tcpClient != null || _tcpStream != null || _tcpListener != null)
            {
                CloseTcpTransport();
            }

            lock (_transportLock)
            {
                _tcpClient = new TcpClient();
                _tcpClient.Connect(host, port);
                _tcpStream = _tcpClient.GetStream();
                _tcpStream.ReadTimeout = Timeout.Infinite;
                _tcpStream.WriteTimeout = 3000;
            }

            _activeTransport = TransportKind.Tcp;
            StartTcpReceiveLoop();
        }

        private bool IsTcpServerMode()
        {
            return comboBox5 != null && comboBox5.SelectedIndex == 1;
        }

        private void StopTcpAcceptLoop()
        {
            try
            {
                if (_tcpAcceptCts != null)
                {
                    _tcpAcceptCts.Cancel();
                }
            }
            catch { }

            try
            {
                if (_tcpListener != null)
                {
                    _tcpListener.Stop();
                }
            }
            catch { }
        }

        private IPAddress ResolveServerBindAddress(string bindText)
        {
            string input = (bindText ?? string.Empty).Trim();
            if (string.IsNullOrEmpty(input) || input == "*" || input.Equals("any", StringComparison.OrdinalIgnoreCase) || input == "0.0.0.0")
            {
                return IPAddress.Any;
            }

            IPAddress ip;
            if (!IPAddress.TryParse(input, out ip))
            {
                throw new ArgumentException("监听IP无效，请输入本机网卡IP或0.0.0.0");
            }

            return ip;
        }

        private void StartTcpServer(string bindIp, int port)
        {
            if (_tcpListener != null || _tcpClient != null || _tcpStream != null)
            {
                CloseTcpTransport();
            }

            IPAddress bindAddress = ResolveServerBindAddress(bindIp);

            lock (_transportLock)
            {
                _tcpListener = new TcpListener(bindAddress, port);
                _tcpListener.Start();
            }

            _tcpAcceptCts = new CancellationTokenSource();
            var token = _tcpAcceptCts.Token;
            _tcpAcceptTask = Task.Run(() => TcpAcceptLoop(token), token);

            AppendStructuredLog("WiFi", "LISTEN", port.ToString(), 0, UpgradeResultCode.OK, $"server-listening:{bindAddress}", Color.DarkGreen);
        }

        private void TcpAcceptLoop(CancellationToken token)
        {
            try
            {
                TcpClient accepted = _tcpListener.AcceptTcpClient();
                if (token.IsCancellationRequested)
                {
                    accepted.Close();
                    return;
                }

                lock (_transportLock)
                {
                    _tcpClient = accepted;
                    _tcpStream = _tcpClient.GetStream();
                    _tcpStream.ReadTimeout = Timeout.Infinite;
                    _tcpStream.WriteTimeout = 3000;
                }

                _activeTransport = TransportKind.Tcp;
                this.BeginInvoke(new Action(() =>
                {
                    button1.Text = "断开TCP";
                }));

                AppendStructuredLog("WiFi", "ACCEPT", "-", 0, UpgradeResultCode.OK, "server-accepted", Color.DarkGreen);
                StartTcpReceiveLoop();
            }
            catch (ObjectDisposedException)
            {
                // 关闭监听时正常退出
            }
            catch (SocketException ex)
            {
                if (!token.IsCancellationRequested)
                {
                    AppendStructuredLog("WiFi", "LISTEN", "-", 0, UpgradeResultCode.DISCONNECTED, ex.Message, Color.DarkOrange);
                }
            }
            catch (Exception ex)
            {
                if (!token.IsCancellationRequested)
                {
                    AppendStructuredLog("WiFi", "LISTEN", "-", 0, UpgradeResultCode.DISCONNECTED, ex.Message, Color.DarkOrange);
                }
            }
            finally
            {
                lock (_transportLock)
                {
                    if (_tcpListener != null)
                    {
                        try { _tcpListener.Stop(); } catch { }
                        _tcpListener = null;
                    }
                }
            }
        }

        private void TcpReceiveLoop(CancellationToken token)
        {
            byte[] buffer = new byte[4096];

            try
            {
                while (!token.IsCancellationRequested)
                {
                    NetworkStream stream;
                    lock (_transportLock)
                    {
                        stream = _tcpStream;
                    }

                    if (stream == null)
                    {
                        break;
                    }

                    int read = stream.Read(buffer, 0, buffer.Length);
                    if (read <= 0)
                    {
                        break;
                    }

                    byte[] chunk = new byte[read];
                    Buffer.BlockCopy(buffer, 0, chunk, 0, read);
                    ProcessIncomingBytes(chunk);
                }
            }
            catch (IOException ex)
            {
                var sockEx = ex.InnerException as SocketException;
                if (sockEx != null)
                {
                    if (sockEx.SocketErrorCode == SocketError.TimedOut || sockEx.SocketErrorCode == SocketError.WouldBlock)
                    {
                        return;
                    }
                }

                if (!token.IsCancellationRequested)
                {
                    AppendStructuredLog("WiFi", "RX", "-", 0, UpgradeResultCode.DISCONNECTED, ex.Message, Color.DarkOrange);
                }
            }
            catch (SocketException ex)
            {
                if (!token.IsCancellationRequested)
                {
                    if (ex.SocketErrorCode != SocketError.TimedOut && ex.SocketErrorCode != SocketError.WouldBlock)
                    {
                        AppendStructuredLog("WiFi", "RX", "-", 0, UpgradeResultCode.DISCONNECTED, ex.Message, Color.DarkOrange);
                    }
                }
            }
            catch (ObjectDisposedException)
            {
                // 正常关闭流程
            }
            catch (Exception ex)
            {
                if (!token.IsCancellationRequested)
                {
                    AppendStructuredLog("WiFi", "RX", "-", 0, UpgradeResultCode.DISCONNECTED, ex.Message, Color.DarkOrange);
                }
            }
            finally
            {
                if (_activeTransport == TransportKind.Tcp)
                {
                    AppendStructuredLog("WiFi", "LINK", "-", 0, UpgradeResultCode.DISCONNECTED, "tcp-disconnected", Color.DarkOrange);
                    _activeTransport = TransportKind.None;
                    lock (_sessionLock)
                    {
                        _pendingSession = null;
                    }
                    _responseEvent.Set();

                    this.BeginInvoke(new Action(() =>
                    {
                        button1.Text = "连接/断开";
                    }));
                    this.BeginInvoke(new Action(() =>
                    {
                        comboBox5_SelectedIndexChanged(comboBox5, EventArgs.Empty);
                    }));
                }
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

        private int GetBackoffDelayMs(int attemptIndex, int minDelayMs, int maxDelayMs)
        {
            if (attemptIndex <= 0)
            {
                return minDelayMs;
            }

            int delay = minDelayMs + attemptIndex * 50;
            return Math.Min(delay, maxDelayMs);
        }

        private CommandSessionResult SendCommandInSingleFlight(
            OtaCommand cmd,
            byte[] data,
            int timeoutMs,
            bool expectEcho = false,
            int maxBusyRetry = 0,
            int busyMinDelayMs = 100,
            int busyMaxDelayMs = 500)
        {
            var result = new CommandSessionResult
            {
                Result = CommandResultKind.None,
                ResponseData = new byte[0]
            };

            _singleFlightLock.Wait();
            var sw = Stopwatch.StartNew();
            try
            {
                int busyCount = 0;
                int retryCount = 0;

                for (int attempt = 0; attempt <= maxBusyRetry; attempt++)
                {
                    while (_responseEvent.WaitOne(0)) { }

                    var pending = new PendingCommandSession
                    {
                        RequestCmd = cmd,
                        ExpectEcho = expectEcho,
                        TimeoutMs = timeoutMs,
                        StartUtc = DateTime.UtcNow,
                        Result = CommandResultKind.None,
                        ResponseData = new byte[0]
                    };

                    lock (_sessionLock)
                    {
                        _pendingSession = pending;
                    }

                    if (!SendProtocolData(cmd, data))
                    {
                        lock (_sessionLock)
                        {
                            _pendingSession = null;
                        }

                        result.Result = CommandResultKind.SendFailed;
                        break;
                    }

                    bool signaled = false;
                    int waited = 0;
                    const int slice = 50;
                    while (waited < timeoutMs)
                    {
                        if (_abortRequested && cmd != OtaCommand.CMD_UPDATE_ABORT)
                        {
                            lock (_sessionLock)
                            {
                                _pendingSession = null;
                            }

                            result.Result = CommandResultKind.Aborted;
                            result.BusyCount = busyCount;
                            result.RetryCount = retryCount;
                            result.ElapsedMs = sw.ElapsedMilliseconds;
                            return result;
                        }

                        int waitMs = Math.Min(slice, timeoutMs - waited);
                        if (_responseEvent.WaitOne(waitMs))
                        {
                            signaled = true;
                            break;
                        }

                        waited += waitMs;
                    }

                    PendingCommandSession finished;
                    lock (_sessionLock)
                    {
                        finished = _pendingSession;
                        _pendingSession = null;
                    }

                    if (!signaled || finished == null || finished.Result == CommandResultKind.None)
                    {
                        result.Result = CommandResultKind.Timeout;
                        break;
                    }

                    result.Result = finished.Result;
                    result.ResponseCmd = finished.ResponseCmd;
                    result.ResponseData = finished.ResponseData ?? new byte[0];

                    if (finished.Result == CommandResultKind.Busy)
                    {
                        busyCount++;
                        if (attempt < maxBusyRetry)
                        {
                            retryCount++;
                            int delayMs = GetBackoffDelayMs(attempt, busyMinDelayMs, busyMaxDelayMs);
                            AppendStructuredLog(_activeTransport.ToString(), cmd.ToString(), "-", retryCount, UpgradeResultCode.BUSY, $"backoff={delayMs}ms", Color.DarkOrange);
                            Thread.Sleep(delayMs);
                            continue;
                        }
                    }

                    break;
                }

                result.BusyCount = busyCount;
                result.RetryCount = retryCount;
                result.ElapsedMs = sw.ElapsedMilliseconds;
                return result;
            }
            finally
            {
                _singleFlightLock.Release();
            }
        }

        private bool HandleSessionResult(OtaCommand cmd, CommandSessionResult result)
        {
            if (result == null)
            {
                AppendStructuredLog(_activeTransport.ToString(), cmd.ToString(), "-", 0, UpgradeResultCode.RETRY_EXHAUSTED, "null-result", Color.Red);
                return false;
            }

            if (result.IsAckLikeSuccess)
            {
                return true;
            }

            UpgradeResultCode code = MapResultCode(result.Result);
            AppendStructuredLog(_activeTransport.ToString(), cmd.ToString(), "-", result.RetryCount, code, result.Result.ToString(), Color.Red);
            return false;
        }

        private void SetUpgradeIdleImmediately(string reason)
        {
            _abortRequested = true;
            _isUpgrading = false;
            lock (_sessionLock)
            {
                _pendingSession = null;
            }
            _responseEvent.Set();

            this.BeginInvoke(new Action(() =>
            {
                button5.Enabled = true;
            }));

            AppendStructuredLog(_activeTransport.ToString(), "UPGRADE-IDLE", "-", 0, UpgradeResultCode.OK, reason, Color.DarkOrange);
        }

        private void SendAbortNow(string reason)
        {
            bool linkOnline = IsTransportOnline(_activeTransport);
            SetUpgradeIdleImmediately(reason);

            if (!linkOnline)
            {
                AppendStructuredLog(_activeTransport.ToString(), OtaCommand.CMD_UPDATE_ABORT.ToString(), "-", 0, UpgradeResultCode.DISCONNECTED, "skip-send-link-offline", Color.DarkOrange);
                return;
            }

            SendAbortIfOnline(reason);
        }

        private bool RunUpgrade(string filePath, uint targetArea)
        {
            DateTime recoverDeadline = DateTime.UtcNow.AddMilliseconds(ReconnectWindowMs);
            int restartAttempt = 0;

            while (true)
            {
                restartAttempt++;
                ResetUpgradeStartState();
                AppendStructuredLog(_activeTransport.ToString(), "CMD_UPDATE_START", "0", restartAttempt - 1, UpgradeResultCode.OK, "full-restart", Color.Black);

                bool startAcked = false;
                int totalPackets = 0;
                int failedPacket = -1;
                string failedReason = "-";
                UpgradeResultCode lastCode = UpgradeResultCode.OK;
                Stopwatch totalSw = Stopwatch.StartNew();

                try
                {
                    byte[] raw = File.ReadAllBytes(filePath);
                    byte[] aligned = PadTo4Bytes(raw);
                    uint crc32 = CalculateCrc32Stm(aligned);

                    this.Invoke(new Action(() =>
                    {
                        label2.Text = $"对齐大小: {aligned.Length} B";
                        label3.Text = $"CRC32: 0x{crc32:X8}";
                        progressBar1.Minimum = 0;
                        progressBar1.Maximum = aligned.Length;
                        progressBar1.Value = 0;
                    }));

                    List<byte> startData = new List<byte>(16);
                    startData.AddRange(BitConverter.GetBytes(targetArea));
                    startData.AddRange(BitConverter.GetBytes(DefaultVersion));
                    startData.AddRange(BitConverter.GetBytes((uint)aligned.Length));
                    startData.AddRange(BitConverter.GetBytes(crc32));

                    var startResult = SendCommandInSingleFlight(OtaCommand.CMD_UPDATE_START, startData.ToArray(), StartEndTimeoutMs, false, 0);
                    lastCode = MapResultCode(startResult.Result);
                    AppendStructuredLog(_activeTransport.ToString(), OtaCommand.CMD_UPDATE_START.ToString(), "0", 0, lastCode, "start", lastCode == UpgradeResultCode.OK ? Color.DarkGreen : Color.Red);
                    if (!HandleSessionResult(OtaCommand.CMD_UPDATE_START, startResult))
                    {
                        failedReason = "START失败";
                        return false;
                    }
                    startAcked = true;

                    int sent = 0;
                    int packetIndex = 0;
                    while (sent < aligned.Length)
                    {
                        if (_abortRequested)
                        {
                            failedReason = "用户触发ABORT";
                            return false;
                        }

                        int len = Math.Min(UpgradeChunkSize, aligned.Length - sent);
                        byte[] chunk = new byte[len];
                        Buffer.BlockCopy(aligned, sent, chunk, 0, len);

                        packetIndex++;
                        totalPackets++;

                        CommandSessionResult dataResult = null;
                        UpgradeResultCode dataCode = UpgradeResultCode.RETRY_EXHAUSTED;
                        bool packetDone = false;

                        for (int retry = 0; retry <= PacketRetryMax; retry++)
                        {
                            dataResult = SendCommandInSingleFlight(OtaCommand.CMD_UPDATE_DATA, chunk, DataTimeoutMs, false, 0);
                            dataCode = MapResultCode(dataResult.Result);
                            AppendStructuredLog(_activeTransport.ToString(), OtaCommand.CMD_UPDATE_DATA.ToString(), sent.ToString(), retry, dataCode, $"pkt={packetIndex}",
                                dataCode == UpgradeResultCode.OK ? Color.DarkGreen : Color.DarkOrange);

                            if (dataResult.IsAckLikeSuccess)
                            {
                                packetDone = true;
                                break;
                            }

                            if (retry < PacketRetryMax)
                            {
                                Thread.Sleep(PacketBackoffMs[Math.Min(retry, PacketBackoffMs.Length - 1)]);
                            }
                        }

                        if (!packetDone)
                        {
                            failedPacket = packetIndex;
                            failedReason = "DATA失败";
                            lastCode = dataCode == UpgradeResultCode.OK ? UpgradeResultCode.RETRY_EXHAUSTED : dataCode;
                            return false;
                        }

                        sent += len;
                        this.Invoke(new Action(() =>
                        {
                            progressBar1.Value = sent;
                        }));
                    }

                    var endResult = SendCommandInSingleFlight(OtaCommand.CMD_UPDATE_END, null, StartEndTimeoutMs, false, 0);
                    lastCode = MapResultCode(endResult.Result);
                    AppendStructuredLog(_activeTransport.ToString(), OtaCommand.CMD_UPDATE_END.ToString(), totalPackets.ToString(), 0, lastCode, "end",
                        lastCode == UpgradeResultCode.OK ? Color.DarkGreen : Color.Red);
                    if (!HandleSessionResult(OtaCommand.CMD_UPDATE_END, endResult))
                    {
                        failedReason = "END失败";
                        return false;
                    }

                    AppendStructuredLog(_activeTransport.ToString(), "UPGRADE", totalPackets.ToString(), restartAttempt - 1, UpgradeResultCode.OK, "completed", Color.DarkGreen);
                    return true;
                }
                catch (Exception ex)
                {
                    lastCode = UpgradeResultCode.DISCONNECTED;
                    failedReason = ex.Message;
                    AppendStructuredLog(_activeTransport.ToString(), "UPGRADE", failedPacket < 0 ? "-" : failedPacket.ToString(), restartAttempt - 1, lastCode, ex.Message, Color.Red);
                }
                finally
                {
                    totalSw.Stop();
                    AppendStructuredLog(_activeTransport.ToString(), "UPGRADE-SUM", failedPacket < 0 ? totalPackets.ToString() : failedPacket.ToString(), restartAttempt - 1,
                        lastCode, $"reason={failedReason},cost={totalSw.ElapsedMilliseconds}ms", Color.DarkBlue);

                    if (!string.Equals(failedReason, "-", StringComparison.Ordinal) && startAcked && !_abortRequested)
                    {
                        if (IsTransportOnline(_activeTransport))
                        {
                            SendAbortIfOnline("upgrade-failed");
                        }
                    }
                }

                if (_activeTransport != TransportKind.Tcp)
                {
                    return false;
                }

                if (DateTime.UtcNow > recoverDeadline)
                {
                    AppendStructuredLog("WiFi", "RECOVER", "-", restartAttempt - 1, UpgradeResultCode.RETRY_EXHAUSTED, "recover-window-timeout", Color.Red);
                    return false;
                }

                bool recovered = TryRecoverTcpConnection(recoverDeadline);
                if (!recovered)
                {
                    AppendStructuredLog("WiFi", "RECOVER", "-", restartAttempt - 1, UpgradeResultCode.DISCONNECTED, "reconnect-failed", Color.Red);
                    return false;
                }
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
            if (_isUpgrading)
            {
                AppendStructuredLog("UART", "CHANNEL_SWITCH", "-", 0, UpgradeResultCode.BUSY, "upgrade-in-progress", Color.DarkOrange);
                MessageBox.Show("升级进行中，禁止手工切换通道。");
                return;
            }

            try
            {
                if (_activeTransport == TransportKind.Tcp)
                {
                    CloseTcpTransport();
                }

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
                    _activeTransport = TransportKind.Serial;
                    button2.Text = "关闭串口";
                    AppendStructuredLog("UART", "OPEN", "-", 0, UpgradeResultCode.OK, "serial-opened", Color.DarkGreen);
                }
                else
                {
                    sp1.Close();
                    if (_activeTransport == TransportKind.Serial)
                    {
                        _activeTransport = TransportKind.None;
                    }
                    button2.Text = "打开串口";
                    AppendStructuredLog("UART", "CLOSE", "-", 0, UpgradeResultCode.OK, "serial-closed", Color.DarkOrange);
                }
            }
            catch (Exception ex)
            {
                MessageBox.Show("串口操作失败： " + ex.Message);
            }
        }

        private void button1_Click(object sender, EventArgs e)
        {
            if (_isUpgrading)
            {
                AppendStructuredLog("WiFi", "CHANNEL_SWITCH", "-", 0, UpgradeResultCode.BUSY, "upgrade-in-progress", Color.DarkOrange);
                MessageBox.Show("升级进行中，禁止手工切换通道。");
                return;
            }

            try
            {
                if (_activeTransport == TransportKind.Tcp || _tcpListener != null)
                {
                    CloseTcpTransport();
                    comboBox5_SelectedIndexChanged(comboBox5, EventArgs.Empty);
                    return;
                }

                if (sp1.IsOpen)
                {
                    sp1.Close();
                    button2.Text = "打开串口";
                }

                int port = int.Parse(textBox2.Text.Trim());

                if (IsTcpServerMode())
                {
                    StartTcpServer(textBox1.Text.Trim(), port);
                    button1.Text = "停止监听";
                    return;
                }

                string host = textBox1.Text.Trim();
                OpenTcpTransport(host, port);
                button1.Text = "断开TCP";
                AppendStructuredLog("WiFi", "CONNECT", port.ToString(), 0, UpgradeResultCode.OK, $"client-connected:{host}", Color.DarkGreen);
            }
            catch (Exception ex)
            {
                AppendStructuredLog("WiFi", "CONNECT", "-", 0, UpgradeResultCode.DISCONNECTED, ex.Message, Color.Red);
                CloseTcpTransport();
                comboBox5_SelectedIndexChanged(comboBox5, EventArgs.Empty);
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

            textBox1.Text = "0.0.0.0";
            textBox2.Text = "5000";

            button1.Click += button1_Click;

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

            // TCP 模式：Client / Server
            comboBox5.Items.Clear();
            comboBox5.Items.AddRange(new string[]
            {
                "Client",
                "Server"
            });
            comboBox5.SelectedIndex = 1;
            comboBox5_SelectedIndexChanged(comboBox5, EventArgs.Empty);

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

            ProcessIncomingBytes(buffer);
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
                            AppendStructuredLog(_activeTransport.ToString(), "RX_TEXT", "-", 0, UpgradeResultCode.OK, line, Color.DarkCyan);
                        }
                        start = i + 1;
                    }
                }

                if (start > 0)
                {
                    _utf8RxBuffer.Remove(0, start);
                }

                if (_utf8RxBuffer.Length >= 120)
                {
                    string tail = _utf8RxBuffer.ToString().Trim();
                    if (!string.IsNullOrWhiteSpace(tail))
                    {
                        AppendStructuredLog(_activeTransport.ToString(), "RX_TEXT", "-", 0, UpgradeResultCode.OK, tail, Color.DarkCyan);
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
                    if (_rxFrameBuffer.Count > 1)
                    {
                        _rxFrameBuffer.RemoveRange(0, _rxFrameBuffer.Count - 1);
                    }
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
                if (len > 1024)
                {
                    AppendStructuredLog(_activeTransport.ToString(), "RX", "-", 0, UpgradeResultCode.LEN_ERR, $"len={len}", Color.Red);
                    _rxFrameBuffer.RemoveAt(0);
                    continue;
                }

                int frameLen = 12 + len;
                if (_rxFrameBuffer.Count < frameLen)
                {
                    return;
                }

                byte[] frame = _rxFrameBuffer.GetRange(0, frameLen).ToArray();
                ushort recvCrc = BitConverter.ToUInt16(frame, frameLen - 2);
                ushort calcCrc = CalculateCRC16(frame.Take(frameLen - 2).ToArray());
                if (recvCrc != calcCrc)
                {
                    AppendStructuredLog(_activeTransport.ToString(), "RX", "-", 0, UpgradeResultCode.CRC_ERR, $"recv=0x{recvCrc:X4},calc=0x{calcCrc:X4}", Color.Red);
                    _rxFrameBuffer.RemoveAt(0);
                    continue;
                }

                _rxFrameBuffer.RemoveRange(0, frameLen);

                ushort cmd = BitConverter.ToUInt16(frame, 2);
                byte[] data = len > 0 ? frame.Skip(6).Take(len).ToArray() : new byte[0];

                bool matchedPending = false;
                PendingCommandSession pending;
                lock (_sessionLock)
                {
                    pending = _pendingSession;
                    if (pending != null)
                    {
                        TimeSpan age = DateTime.UtcNow - pending.StartUtc;
                        bool inWindow = age.TotalMilliseconds >= 0 && age.TotalMilliseconds <= pending.TimeoutMs + 200;
                        bool cmdMatched = cmd == (ushort)OtaCommand.CMD_ACK
                            || cmd == (ushort)OtaCommand.CMD_NACK
                            || cmd == (ushort)OtaCommand.CMD_BUSY
                            || (pending.ExpectEcho && cmd == (ushort)pending.RequestCmd);

                        if (inWindow && cmdMatched)
                        {
                            matchedPending = true;
                            pending.ResponseCmd = cmd;
                            pending.ResponseData = data;

                            if (cmd == (ushort)OtaCommand.CMD_ACK)
                            {
                                pending.Result = CommandResultKind.Ack;
                            }
                            else if (cmd == (ushort)OtaCommand.CMD_NACK)
                            {
                                pending.Result = CommandResultKind.Nack;
                            }
                            else if (cmd == (ushort)OtaCommand.CMD_BUSY)
                            {
                                pending.Result = CommandResultKind.Busy;
                            }
                            else
                            {
                                pending.Result = CommandResultKind.Echo;
                            }

                            _responseEvent.Set();
                        }
                    }
                }

                string cmdName = ((OtaCommand)cmd).ToString();
                UpgradeResultCode rxCode = cmd == (ushort)OtaCommand.CMD_NACK ? UpgradeResultCode.NACK
                    : cmd == (ushort)OtaCommand.CMD_BUSY ? UpgradeResultCode.BUSY
                    : UpgradeResultCode.OK;
                AppendStructuredLog(_activeTransport.ToString(), cmdName, len.ToString(), 0, rxCode, matchedPending ? "matched" : "unmatched", Color.DarkCyan);

                if (cmd == (ushort)OtaCommand.CMD_ACK)
                {
                    TryParseAckData(data);
                }
            }
        }

        private void TryParseAckData(byte[] data)
        {
            if (data == null || data.Length == 0)
            {
                return;
            }

            if (data.Length == 24)
            {
                uint bootVersion = BitConverter.ToUInt32(data, 0);
                uint appVersion = BitConverter.ToUInt32(data, 4);
                uint hwVersion = BitConverter.ToUInt32(data, 8);
                string buildDate = Encoding.ASCII.GetString(data, 12, 12).TrimEnd('\0', ' ');
                AppendStructuredLog(_activeTransport.ToString(), "ACK_VER", "24", 0, UpgradeResultCode.OK,
                    $"boot={bootVersion},app={appVersion},hw={hwVersion},build={buildDate}", Color.DarkSlateBlue);
                return;
            }

            if (data.Length == 14)
            {
                uint bootVersion = BitConverter.ToUInt32(data, 0);
                uint bootRunCount = BitConverter.ToUInt32(data, 4);
                uint runAppVersion = BitConverter.ToUInt32(data, 8);
                byte appAStatus = data[12];
                byte appBStatus = data[13];
                AppendStructuredLog(_activeTransport.ToString(), "ACK_PARAM", "14", 0, UpgradeResultCode.OK,
                    $"boot={bootVersion},runCnt={bootRunCount},runApp={runAppVersion},A={appAStatus},B={appBStatus}", Color.DarkSlateBlue);
                return;
            }

            AppendStructuredLog(_activeTransport.ToString(), "ACK_DATA", data.Length.ToString(), 0, UpgradeResultCode.OK, "ack-payload", Color.DarkSlateBlue);
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
            SendAbortNow("用户点击中止");
        }

        private void button7_Click_3(object sender, EventArgs e)
        {
            int selectedIndex = comboBox3.SelectedIndex;
            Task.Run(() =>
            {
                if (selectedIndex == 0)
                {
                    var ping = SendCommandInSingleFlight(OtaCommand.CMD_PING, Encoding.ASCII.GetBytes("PING"), 2000, true, 0);
                    HandleSessionResult(OtaCommand.CMD_PING, ping);
                }
                else if (selectedIndex == 1)
                {
                    var reset = SendCommandInSingleFlight(OtaCommand.CMD_RESET, null, 2000, false, 0);
                    HandleSessionResult(OtaCommand.CMD_RESET, reset);
                }
                else if (selectedIndex == 2)
                {
                    var ver = SendCommandInSingleFlight(OtaCommand.CMD_GET_VERSION, null, 2000, false, 0);
                    HandleSessionResult(OtaCommand.CMD_GET_VERSION, ver);
                }
                else if (selectedIndex == 3)
                {
                    var param = SendCommandInSingleFlight(OtaCommand.CMD_PARAM_READ, null, 2000, false, 0);
                    HandleSessionResult(OtaCommand.CMD_PARAM_READ, param);
                }
            });
        }

        private async void button5_Click(object sender, EventArgs e)
        {
            if (_isUpgrading)
            {
                MessageBox.Show("正在升级中，请勿重复点击。");
                return;
            }

            if (_activeTransport == TransportKind.None)
            {
                MessageBox.Show("请先打开串口或连接TCP。");
                return;
            }

            if (string.IsNullOrWhiteSpace(textBox3.Text) || !File.Exists(textBox3.Text))
            {
                MessageBox.Show("请先选择有效的 app.bin 文件。");
                return;
            }

            TransportKind owner = _activeTransport;
            if (!TryAcquireUpgradeSession(owner))
            {
                MessageBox.Show($"当前已有升级会话占用：{_upgradeSessionOwner}");
                return;
            }

            uint targetArea = GetSelectedTargetArea();

            _isUpgrading = true;
            _abortRequested = false;
            button5.Enabled = false;

            bool ok = false;
            try
            {
                ok = await Task.Run(() => RunUpgrade(textBox3.Text, targetArea));
            }
            finally
            {
                _isUpgrading = false;
                button5.Enabled = true;
                ReleaseUpgradeSession(owner);
            }

            MessageBox.Show(ok ? "升级成功" : "升级失败，请查看日志");

            if (ok)
            {
                await Task.Delay(200);
                await Task.Run(() =>
                {
                    var resetResult = SendCommandInSingleFlight(OtaCommand.CMD_RESET, null, 2000, false, 0);
                    HandleSessionResult(OtaCommand.CMD_RESET, resetResult);
                });
            }
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

        private void tabPage1_Click(object sender, EventArgs e)
        {

        }

        private void comboBox5_SelectedIndexChanged(object sender, EventArgs e)
        {
            if (_suppressTcpModeChangeEvent)
            {
                return;
            }

            if (_isUpgrading)
            {
                _suppressTcpModeChangeEvent = true;
                comboBox5.SelectedIndex = _lastTcpModeIndex;
                _suppressTcpModeChangeEvent = false;
                AppendStructuredLog("WiFi", "MODE_SWITCH", "-", 0, UpgradeResultCode.BUSY, "blocked-during-upgrade", Color.DarkOrange);
                return;
            }

            _lastTcpModeIndex = comboBox5.SelectedIndex;
            bool isServer = IsTcpServerMode();

            // Client模式为远端IP；Server模式为本地监听IP（支持0.0.0.0/any）
            textBox1.Enabled = true;

            if (_activeTransport == TransportKind.Tcp || _tcpListener != null)
            {
                button1.Text = isServer ? "停止监听" : "断开TCP";
                return;
            }

            button1.Text = isServer ? "开始监听" : "连接TCP";
            AppendStructuredLog("WiFi", "MODE_SWITCH", isServer ? "Server" : "Client", 0, UpgradeResultCode.OK, "mode-updated", Color.Gray);
        }

        private void AppendStructuredLog(string transport, string cmd, string seqOrOffset, int retry, UpgradeResultCode result, string reason, Color color)
        {
            string ts = DateTime.Now.ToString("HH:mm:ss.fff");
            AppendLog($"[{ts}] transport={transport}, cmd={cmd}, seq/offset={seqOrOffset}, retry={retry}, result={result}, reason={reason}", color);
        }

        private bool TryAcquireUpgradeSession(TransportKind owner)
        {
            lock (_upgradeSessionLock)
            {
                if (_upgradeSessionOwner != TransportKind.None && _upgradeSessionOwner != owner)
                {
                    return false;
                }

                _upgradeSessionOwner = owner;
                return true;
            }
        }

        private void ReleaseUpgradeSession(TransportKind owner)
        {
            lock (_upgradeSessionLock)
            {
                if (_upgradeSessionOwner == owner)
                {
                    _upgradeSessionOwner = TransportKind.None;
                }
            }
        }

        private void ResetUpgradeStartState()
        {
            _abortRequested = false;
            lock (_sessionLock)
            {
                _pendingSession = null;
            }

            while (_responseEvent.WaitOne(0)) { }
        }

        private bool IsTransportOnline(TransportKind kind)
        {
            if (kind == TransportKind.Serial)
            {
                return sp1 != null && sp1.IsOpen;
            }

            if (kind == TransportKind.Tcp)
            {
                lock (_transportLock)
                {
                    return _tcpClient != null && _tcpClient.Connected && _tcpStream != null;
                }
            }

            return false;
        }

        private bool TryRecoverTcpConnection(DateTime deadlineUtc)
        {
            for (int i = 0; i < ReconnectRetryMax; i++)
            {
                if (DateTime.UtcNow > deadlineUtc)
                {
                    return false;
                }

                try
                {
                    int port = int.Parse(textBox2.Text.Trim());
                    if (IsTcpServerMode())
                    {
                        if (_tcpListener == null)
                        {
                            StartTcpServer(textBox1.Text.Trim(), port);
                        }

                        DateTime waitUntil = DateTime.UtcNow.AddMilliseconds(ReconnectIntervalMs);
                        while (DateTime.UtcNow < waitUntil)
                        {
                            if (IsTransportOnline(TransportKind.Tcp))
                            {
                                AppendStructuredLog("WiFi", "RECONNECT", "-", i + 1, UpgradeResultCode.OK, "server accepted", Color.DarkGreen);
                                return true;
                            }

                            Thread.Sleep(100);
                        }
                    }
                    else
                    {
                        string host = textBox1.Text.Trim();
                        OpenTcpTransport(host, port);
                        if (IsTransportOnline(TransportKind.Tcp))
                        {
                            AppendStructuredLog("WiFi", "RECONNECT", "-", i + 1, UpgradeResultCode.OK, "client connected", Color.DarkGreen);
                            return true;
                        }
                    }
                }
                catch (Exception ex)
                {
                    AppendStructuredLog("WiFi", "RECONNECT", "-", i + 1, UpgradeResultCode.DISCONNECTED, ex.Message, Color.DarkOrange);
                }

                Thread.Sleep(ReconnectIntervalMs);
            }

            return false;
        }

        private void SendAbortIfOnline(string reason)
        {
            if (!IsTransportOnline(_activeTransport))
            {
                AppendStructuredLog(_activeTransport.ToString(), "CMD_UPDATE_ABORT", "-", 0, UpgradeResultCode.DISCONNECTED, reason, Color.DarkOrange);
                return;
            }

            Task.Run(() =>
            {
                var abortResult = SendCommandInSingleFlight(OtaCommand.CMD_UPDATE_ABORT, null, StartEndTimeoutMs, false, 0);
                UpgradeResultCode code = MapResultCode(abortResult.Result);
                AppendStructuredLog(_activeTransport.ToString(), "CMD_UPDATE_ABORT", "-", 0, code, reason, code == UpgradeResultCode.OK ? Color.DarkGreen : Color.DarkOrange);
            });
        }

        private UpgradeResultCode MapResultCode(CommandResultKind result)
        {
            switch (result)
            {
                case CommandResultKind.Ack:
                case CommandResultKind.Echo:
                    return UpgradeResultCode.OK;
                case CommandResultKind.Nack:
                    return UpgradeResultCode.NACK;
                case CommandResultKind.Busy:
                    return UpgradeResultCode.BUSY;
                case CommandResultKind.Timeout:
                    return UpgradeResultCode.TIMEOUT;
                case CommandResultKind.SendFailed:
                    return UpgradeResultCode.DISCONNECTED;
                default:
                    return UpgradeResultCode.RETRY_EXHAUSTED;
            }
        }

    }
}
