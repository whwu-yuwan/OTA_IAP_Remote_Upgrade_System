using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Data;
using System.Drawing;
using System.IO.Ports;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace IAP_OTA_Remote_Upgrade_UpperComputer
{
    public partial class Form1 : Form
    {
        public const UInt32 HEADER = 0xAA55;


        SerialPort sp1 = new SerialPort();
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
            sp1.Write ("");
        }

        private void textBox3_TextChanged(object sender, EventArgs e)
        {

        }

        private void textBox2_TextChanged(object sender, EventArgs e)
        {

        }

        private void button3_Click(object sender, EventArgs e)
        {

        }

        private void comboBox1_SelectedIndexChanged(object sender, EventArgs e)
        {

        }

        private void button2_Click(object sender, EventArgs e)
        {
            try
            {
                if (sp1.IsOpen)
                {
                    sp1.PortName = comboBox2.Text;
                    sp1.BaudRate = int.Parse(comboBox1.Text);
                    sp1.DataBits = 8;
                    sp1.StopBits = 0;
                    sp1.Parity = Parity.None;

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

            // 默认选中 115200
            comboBox1.SelectedIndex = 4;

            comboBox2.Items.AddRange(SerialPort.GetPortNames());
        }
    }
}
