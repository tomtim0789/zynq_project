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
#include "xusbps_COMMS.h" // ������������USBͨ��ģ��ͷ�ļ�
#include "xil_io.h"       // ����DDR��д

#define USB_TX_BUFFER_SIZE 512       // USB���ͻ�������С
#define RINGBUFFER_SIZE (64 * 1024)  // USB���λ�������С

// DDR��ȡ����
#define DDR_START_ADDRESS 0x10000000   // DDR��ȡ��ʼ��ַ
#define DDR_READ_SIZE     512          // ÿ�δ�DDR��ȡ���ֽ��� (������USB_TX_BUFFER_SIZE��ͬ���С)

static XScuGic IntcInstance;
static XUsbPs UsbInstance;

// DDR��ȡ����
// ����:
//   buffer: �洢��ȡ���ݵĻ�����
//   read_bytes: ��Ҫ��ȡ���ֽ���
// ����:
//   ʵ�ʶ�ȡ���ֽ���
u32 read_data_from_ddr(u8 *buffer, u32 read_bytes) {
    static u32 current_ddr_offset = 0; // ��̬��������¼DDR��ȡ�ĵ�ǰƫ��

    // ȷ����ȡ���ֽ���������DDR_READ_SIZE��buffer��ʵ�ʴ�С
    u32 bytes_to_read = (read_bytes > DDR_READ_SIZE) ? DDR_READ_SIZE : read_bytes;

    // ����DDR�������ģ��������Ǵ�DDR_START_ADDRESS��ʼ��
    // ����򵥵�ѭ����ȡ��ÿ�ζ�ȡ8�ֽڣ�64λ��
    for (int i = 0; i < bytes_to_read; i += 8) {
        // ����Ƿ񳬳�DDR��ַ��Χ�����߻������Ƿ���
        if ((current_ddr_offset + i) >= (4096 - 8) || (i + 8) > bytes_to_read) { // ����DDR�ܴ�СΪ4096���������˼򵥼��
            break;
        }
        u64 data64 = Xil_In64(DDR_START_ADDRESS + current_ddr_offset + i);
        // ��64λ���ݲ��Ϊ8���ֽڴ洢��u8��������
        for (int j = 0; j < 8; j++) {
            if ((i + j) < bytes_to_read) {
                buffer[i + j] = (u8)((data64 >> (j * 8)) & 0xFF);
            }
        }
    }

    // ������һ�ζ�ȡ��ƫ��
    current_ddr_offset += bytes_to_read;
    // ����ﵽ��DDR��ĳ�����ƣ����Կ�������ƫ�ƻ�ѭ����ȡ
    // ����򵥵���ʾ�������Ը�����Ҫ����
    if (current_ddr_offset >= 4096) { // ����DDR�ɶ�ȡ�ܴ�СΪ4096�ֽ�
        current_ddr_offset = 0;
    }

    return bytes_to_read;
}


int main(void)
{
    int Status;
    u8 usb_tx_buffer[USB_TX_BUFFER_SIZE]; // ����USB���͵Ļ�����
    u8 usb_rx_buffer[USB_TX_BUFFER_SIZE]; // ����USB���յĻ����� (�����Ҫ)

    Xil_DCacheDisable(); // �������ݻ��棬��DDR��дʱ�Ļ���һ��������

    xil_printf("Zynq-7010 DDR Read and USB Send Example Started\r\n");

    // ȷ��USB PHY���ڸ�λ״̬
    volatile u32 *gpio_base = (u32 *)0xE000A000;
    volatile u32 *gpio_oen = (u32 *)0xE000A208;
    volatile u32 *gpio_dir = (u32 *)0xE000A204;

    *(gpio_oen) |= 0x00000080;  // �������
    *(gpio_dir) |= 0x00000080;  // ��������Ϊ���
    *gpio_base = 0xff7f0080;    // ����MIO7Ϊ1

    // ��λUSB����
    u32 *usb0_base = (u32 *)0xE0002140;  // usb0 + cmdƫ����
    *usb0_base = 0x00080002;             // ��λֵ + ��λλ��Ϊ1

    while((*usb0_base & 0x2) > 0) {      // �ȴ���λ���
        // ��ѭ���ȴ�
    }

    // ��ʼ��USBͨ��
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
        // ----------------- DDR ��ȡ���� -----------------
        u32 bytes_read_from_ddr = read_data_from_ddr(usb_tx_buffer, DDR_READ_SIZE);

        if (bytes_read_from_ddr > 0) {
            xil_printf("Read %d bytes from DDR at 0x%08X\r\n", bytes_read_from_ddr, DDR_START_ADDRESS);

            // ----------------- USB ���Ͳ��� -----------------
            u32 bytes_sent = xusb_COMMS_send_data(&UsbInstance, usb_tx_buffer, bytes_read_from_ddr);

            if (bytes_sent == bytes_read_from_ddr) {
                xil_printf("Sent %d bytes to USB successfully.\r\n", bytes_sent);
            } else {
                xil_printf("Error: Sent %d/%d bytes to USB.\r\n", bytes_sent, bytes_read_from_ddr);
            }
        }

        // ----------------- USB ���ղ��� (��ѡ���������ع���) -----------------
        u32 bytes_available_usb = xusb_COMMS_rx_bytes_available();

        if (bytes_available_usb > 0) {
            // ���ƶ�ȡ��С��ֹ���������
            u32 bytes_to_read_usb = (bytes_available_usb > USB_TX_BUFFER_SIZE) ? USB_TX_BUFFER_SIZE : bytes_available_usb;

            // ��������
            u32 bytes_received_usb = xusb_COMMS_receive_data(usb_rx_buffer, bytes_to_read_usb);

            if (bytes_received_usb > 0) {
                xil_printf("Received %d bytes from USB (loopback data).\r\n", bytes_received_usb);
                // �����������ﴦ����յ������ݣ������ӡ������ִ����������
            }
        }

        // ���С�ӳ��Լ���CPU���أ���ֹæ��
        usleep(50000); // �����ӳ�ʱ���Կ���DDR��ȡ��USB���͵�����
    }

    return XST_SUCCESS;
}
