#include "xparameters.h"
#include "xusbps.h"
#include "xscugic.h"
#include "xusbps_ch9.h"
#include "xil_exception.h"
#include "xpseudo_asm.h"
#include "xreg_cortexa9.h"
#include "xil_cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xusbps_COMMS.h" // 假设这是您的USB通信模块头文件
#include "xil_io.h"       // 用于DDR读写

#define USB_TX_BUFFER_SIZE 512       // USB发送缓冲区大小
#define RINGBUFFER_SIZE (64 * 1024)  // USB环形缓冲区大小

// DDR读取配置
#define DDR_START_ADDRESS 0x10000000   // DDR读取起始地址
#define DDR_READ_SIZE     512          // 每次从DDR读取的字节数 (建议与USB_TX_BUFFER_SIZE相同或更小)

static XScuGic IntcInstance;
static XUsbPs UsbInstance;

// DDR读取函数
// 参数:
//   buffer: 存储读取数据的缓冲区
//   read_bytes: 需要读取的字节数
// 返回:
//   实际读取的字节数
u32 read_data_from_ddr(u8 *buffer, u32 read_bytes) {
    static u32 current_ddr_offset = 0; // 静态变量，记录DDR读取的当前偏移

    // 确保读取的字节数不超过DDR_READ_SIZE和buffer的实际大小
    u32 bytes_to_read = (read_bytes > DDR_READ_SIZE) ? DDR_READ_SIZE : read_bytes;

    // 假设DDR是连续的，并且我们从DDR_START_ADDRESS开始读
    // 这里简单地循环读取，每次读取8字节（64位）
    for (int i = 0; i < bytes_to_read; i += 8) {
        // 检查是否超出DDR地址范围，或者缓冲区是否满
        if ((current_ddr_offset + i) >= (4096 - 8) || (i + 8) > bytes_to_read) { // 假设DDR总大小为4096，这里做了简单检查
            break;
        }
        u64 data64 = Xil_In64(DDR_START_ADDRESS + current_ddr_offset + i);
        // 将64位数据拆分为8个字节存储到u8缓冲区中
        for (int j = 0; j < 8; j++) {
            if ((i + j) < bytes_to_read) {
                buffer[i + j] = (u8)((data64 >> (j * 8)) & 0xFF);
            }
        }
    }

    // 更新下一次读取的偏移
    current_ddr_offset += bytes_to_read;
    // 如果达到了DDR的某个限制，可以考虑重置偏移或循环读取
    // 这里简单地演示，您可以根据需要调整
    if (current_ddr_offset >= 4096) { // 假设DDR可读取总大小为4096字节
        current_ddr_offset = 0;
    }

    return bytes_to_read;
}


int main(void)
{
    int Status;
    u8 usb_tx_buffer[USB_TX_BUFFER_SIZE]; // 用于USB发送的缓冲区
    u8 usb_rx_buffer[USB_TX_BUFFER_SIZE]; // 用于USB接收的缓冲区 (如果需要)

    Xil_DCacheDisable(); // 禁用数据缓存，简化DDR读写时的缓存一致性问题

    xil_printf("Zynq-7010 DDR Read and USB Send Example Started\r\n");

    // 确保USB PHY不在复位状态
    volatile u32 *gpio_base = (u32 *)0xE000A000;
    volatile u32 *gpio_oen = (u32 *)0xE000A208;
    volatile u32 *gpio_dir = (u32 *)0xE000A204;

    *(gpio_oen) |= 0x00000080;  // 启用输出
    *(gpio_dir) |= 0x00000080;  // 设置引脚为输出
    *gpio_base = 0xff7f0080;    // 更新MIO7为1

    // 复位USB外设
    u32 *usb0_base = (u32 *)0xE0002140;  // usb0 + cmd偏移量
    *usb0_base = 0x00080002;             // 复位值 + 复位位设为1

    while((*usb0_base & 0x2) > 0) {      // 等待复位完成
        // 空循环等待
    }

    // 初始化USB通信
    Status = xusb_COMMS_init(&IntcInstance, &UsbInstance,
                           XPAR_XUSBPS_0_DEVICE_ID,
                           XPAR_XUSBPS_0_INTR,
                           RINGBUFFER_SIZE);
    if (Status != XST_SUCCESS) {
        xil_printf("USB Initialization Failed\r\n");
        return XST_FAILURE;
    }

    xil_printf("USB Initialized Successfully\r\n");
    xil_printf("Starting DDR read and USB send loop...\r\n");

    while(1) {
        // ----------------- DDR 读取部分 -----------------
        u32 bytes_read_from_ddr = read_data_from_ddr(usb_tx_buffer, DDR_READ_SIZE);

        if (bytes_read_from_ddr > 0) {
            xil_printf("Read %d bytes from DDR at 0x%08X\r\n", bytes_read_from_ddr, DDR_START_ADDRESS);

            // ----------------- USB 发送部分 -----------------
            u32 bytes_sent = xusb_COMMS_send_data(&UsbInstance, usb_tx_buffer, bytes_read_from_ddr);

            if (bytes_sent == bytes_read_from_ddr) {
                xil_printf("Sent %d bytes to USB successfully.\r\n", bytes_sent);
            } else {
                xil_printf("Error: Sent %d/%d bytes to USB.\r\n", bytes_sent, bytes_read_from_ddr);
            }
        }

        // ----------------- USB 接收部分 (可选，保留环回功能) -----------------
        u32 bytes_available_usb = xusb_COMMS_rx_bytes_available();

        if (bytes_available_usb > 0) {
            // 限制读取大小防止缓冲区溢出
            u32 bytes_to_read_usb = (bytes_available_usb > USB_TX_BUFFER_SIZE) ? USB_TX_BUFFER_SIZE : bytes_available_usb;

            // 接收数据
            u32 bytes_received_usb = xusb_COMMS_receive_data(usb_rx_buffer, bytes_to_read_usb);

            if (bytes_received_usb > 0) {
                xil_printf("Received %d bytes from USB (loopback data).\r\n", bytes_received_usb);
                // 您可以在这里处理接收到的数据，例如打印出来或执行其他操作
            }
        }

        // 添加小延迟以减少CPU负载，防止忙等
        usleep(50000); // 调整延迟时间以控制DDR读取和USB发送的速率
    }

    return XST_SUCCESS;
}
