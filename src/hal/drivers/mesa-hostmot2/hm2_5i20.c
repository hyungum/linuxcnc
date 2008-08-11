
//
//    Copyright (C) 2007-2008 Sebastian Kuzminsky
//
//    This program is free software; you can redistribute it and/or modify
//    it under the terms of the GNU General Public License as published by
//    the Free Software Foundation; either version 2 of the License, or
//    (at your option) any later version.
//
//    This program is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//    GNU General Public License for more details.
//
//    You should have received a copy of the GNU General Public License
//    along with this program; if not, write to the Free Software
//    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
//


#include <linux/pci.h>

#include "rtapi.h"
#include "rtapi_app.h"
#include "rtapi_string.h"

#include "hal.h"

#include "hostmot2-lowlevel.h"
#include "hm2_5i20.h"


MODULE_LICENSE("GPL");
MODULE_AUTHOR("Sebastian Kuzminsky");
MODULE_DESCRIPTION("Driver for HostMot2 on the 5i20 Mesa Anything I/O board");
MODULE_SUPPORTED_DEVICE("Mesa-AnythingIO-5i20");


// #if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,22)
//     #error "Kernel version not supported, 2.6.22 and later only" 
// #endif


static char *config[HM2_5I20_MAX_BOARDS];
static int num_config_strings = HM2_5I20_MAX_BOARDS;
module_param_array(config, charp, &num_config_strings, S_IRUGO);
MODULE_PARM_DESC(config, "config string for 5i20 boards (see hostmot2(9) manpage)");


static int comp_id;


// FIXME: should probably have a linked list of boards instead of an array
static hm2_5i20_t hm2_5i20_board[HM2_5I20_MAX_BOARDS];
static int num_boards = 0;


static struct pci_device_id hm2_5i20_pci_tbl[] = {
    {
        .vendor = 0x10b5,
        .device = 0x9030,
        .subvendor = 0x10b5,
        .subdevice = 0x3131,
    },

    {0,},
};

MODULE_DEVICE_TABLE(pci, hm2_5i20_pci_tbl);




// 
// these are the "low-level I/O" functions exported up
//

static int hm2_5i20_read(hm2_lowlevel_io_t *this, u32 addr, void *buffer, int size) {
    hm2_5i20_t *board = this->private;
    memcpy(buffer, (board->base + addr), size);
    return 1;  // success
}

static int hm2_5i20_write(hm2_lowlevel_io_t *this, u32 addr, void *buffer, int size) {
    hm2_5i20_t *board = this->private;
    memcpy((board->base + addr), buffer, size);
    return 1;  // success
}


static int hm2_5i20_program_fpga(hm2_lowlevel_io_t *this, const bitfile_t *bitfile) {
    hm2_5i20_t *board = this->private;
    int i;
    u32 status, control;

    // set /WRITE low for data transfer, and turn on LED
    status = inl(board->ctrl_base_addr + CTRL_STAT_OFFSET);
    control = status & ~_WRITE_MASK & ~_LED_MASK;
    outl(control, board->ctrl_base_addr + CTRL_STAT_OFFSET);

    // program the FPGA
    for (i = 0; i < bitfile->e.size; i ++) {
        outb(bitfile->e.data[i], board->data_base_addr);
    }

    // all bytes transferred, make sure FPGA is all set up now
    status = inl(board->ctrl_base_addr + CTRL_STAT_OFFSET);
    if (!(status & _INIT_MASK)) {
	// /INIT goes low on CRC error
	THIS_ERR("FPGA asserted /INIT: CRC error\n");
        goto fail;
    }
    if (!(status & DONE_MASK)) {
	THIS_ERR("FPGA did not assert DONE\n");
	goto fail;
    }

    // turn off write enable and LED
    control = status | _WRITE_MASK | _LED_MASK;
    outl(control, board->ctrl_base_addr + CTRL_STAT_OFFSET);

    THIS_INFO("Successfully programmed %u bytes\n", bitfile->e.size);
    return 0;


fail:
    // set /PROGRAM low (reset device), /WRITE high and LED off
    status = inl(board->ctrl_base_addr + CTRL_STAT_OFFSET);
    control = status & ~_PROGRAM_MASK;
    control |= _WRITE_MASK | _LED_MASK;
    outl(control, board->ctrl_base_addr + CTRL_STAT_OFFSET);
    return -EIO;
}


static int hm2_5i20_reset(hm2_lowlevel_io_t *this) {
    hm2_5i20_t *board = this->private;
    u32 status;
    u32 control;

    status = inl(board->ctrl_base_addr + CTRL_STAT_OFFSET);

    // set /PROGRAM bit low to reset the FPGA
    control = status & ~_PROGRAM_MASK;

    // set /WRITE and /LED high (idle state)
    control |= _WRITE_MASK | _LED_MASK;

    // and write it back
    outl(control, board->ctrl_base_addr + CTRL_STAT_OFFSET);

    // verify that /INIT and DONE went low
    status = inl(board->ctrl_base_addr + CTRL_STAT_OFFSET);
    if (status & (DONE_MASK | _INIT_MASK)) {
	THIS_ERR(
            "FPGA did not reset: /INIT = %d, DONE = %d\n",
	    (status & _INIT_MASK ? 1 : 0),
            (status & DONE_MASK ? 1 : 0)
        );
	return -EIO;
    }

    // set /PROGRAM high, let FPGA come out of reset
    control = status | _PROGRAM_MASK;
    outl(control, board->ctrl_base_addr + CTRL_STAT_OFFSET);

    // wait for /INIT to go high when it finishes clearing memory
    // This should take no more than 100uS.  If we assume each PCI read
    // takes 30nS (one PCI clock), that is 3300 reads.  Reads actually
    // take several clocks, but even at a microsecond each, 3.3mS is not
    // an excessive timeout value
    {
        int count = 3300;

        do {
            status = inl(board->ctrl_base_addr + CTRL_STAT_OFFSET);
            if (status & _INIT_MASK) break;
        } while (count-- > 0);

        if (count == 0) {
            THIS_ERR("FPGA did not come out of /INIT");
            return -EIO;
        }
    }

    return 0;
}




// 
// misc internal functions
//


static int hm2_5i20_probe(struct pci_dev *dev, const struct pci_device_id *id) {
    int r;
    hm2_5i20_t *board;
    hm2_lowlevel_io_t *this;


    if (num_boards >= HM2_5I20_MAX_BOARDS) {
        LL_WARN("skipping 5i20 at %s, this driver can only handle %d\n", pci_name(dev), HM2_5I20_MAX_BOARDS);
        return -EINVAL;
    }

    // NOTE: this enables the board's BARs -- this fixes the Arty bug
    if (pci_enable_device(dev)) {
        LL_WARN("skipping 5i20 at %s, failed to enable PCI device\n", pci_name(dev));
        return -ENODEV;
    }


    board = &hm2_5i20_board[num_boards];
    this = &board->llio;
    memset(this, 0, sizeof(hm2_lowlevel_io_t));

    snprintf(board->llio.name, HAL_NAME_LEN, "%s.%d", HM2_LLIO_NAME, num_boards);

    THIS_INFO("discovered 5i20 at %s\n", pci_name(dev));


    // 
    // get a hold of the IO resources we'll need later
    //

    // FIXME: should request_region here
    board->ctrl_base_addr = pci_resource_start(dev, 1);
    board->data_base_addr = pci_resource_start(dev, 2);

    // BAR 5 is 64K mem (32 bit)
    board->len = pci_resource_len(dev, 5);
    board->base = ioremap_nocache(pci_resource_start(dev, 5), board->len);
    if (board->base == NULL) {
        THIS_ERR("could not map in FPGA address space\n");
        r = -ENODEV;
        goto fail0;
    }


    //
    // fix up LASxBRD READY if needed
    //
    {
        int offsets[] = { LAS0BRD_OFFSET, LAS1BRD_OFFSET, LAS2BRD_OFFSET, LAS3BRD_OFFSET };
        int i;

        for (i = 0; i < 4; i ++) {
            u32 val;
            int addr = board->ctrl_base_addr + offsets[i];

            val = inl(addr);
            if (!(val & LASxBRD_READY)) {
                THIS_INFO("LAS%dBRD #READY is off, enabling now\n", i);
                val |= LASxBRD_READY;
                outl(val, addr);
            }
        }
    }


    board->dev = dev;

    pci_set_drvdata(dev, board);

    board->llio.comp_id = comp_id;
    board->llio.private = board;

    board->llio.num_ioport_connectors = 3;
    board->llio.ioport_connector_name[0] = "P2";
    board->llio.ioport_connector_name[1] = "P3";
    board->llio.ioport_connector_name[2] = "P4";

    board->llio.fpga_part_number = "2s200pq208";

    board->llio.threadsafe = 1;

    board->llio.read = hm2_5i20_read;
    board->llio.write = hm2_5i20_write;
    board->llio.program_fpga = hm2_5i20_program_fpga;
    board->llio.reset = hm2_5i20_reset;

    r = hm2_register(&board->llio, config[num_boards]);
    if (r != 0) {
        THIS_ERR("board fails HM2 registration\n");
        goto fail1;
    }

    THIS_INFO("found 5i20 board at %s with HostMot2 firmware\n", pci_name(dev));

    num_boards ++;
    return 0;


fail1:
    pci_set_drvdata(dev, NULL);
    iounmap(board->base);
    board->base = NULL;

fail0:
    pci_disable_device(dev);
    return r;
}


static void hm2_5i20_remove(struct pci_dev *dev) {
    int i;

    for (i = 0; i < num_boards; i++) {
        hm2_5i20_t *board = &hm2_5i20_board[i];
        hm2_lowlevel_io_t *this = &board->llio;

        if (board->dev == dev) {
            THIS_INFO("dropping 5i20 at %s\n", pci_name(dev));

            hm2_unregister(&board->llio);

            // Unmap board memory
            if (board->base != NULL) {
                iounmap(board->base);
                board->base = NULL;
            }

            pci_disable_device(dev);
            pci_set_drvdata(dev, NULL);
            board->dev = NULL;
        }
    }
}


static struct pci_driver hm2_5i20_driver = {
	.name = HM2_LLIO_NAME,
	.id_table = hm2_5i20_pci_tbl,
	.probe = hm2_5i20_probe,
	.remove = hm2_5i20_remove,
};


int rtapi_app_main(void) {
    int r = 0;

    LL_INFO("loading HostMot2 Mesa 5i20 driver version " HM2_5I20_VERSION "\n");

    comp_id = hal_init(HM2_LLIO_NAME);
    if (comp_id < 0) return comp_id;

    r = pci_register_driver(&hm2_5i20_driver);
    if (r != 0) {
        LL_ERR("error registering PCI driver\n");
        hal_exit(comp_id);
        return -EINVAL;
    }

    hal_ready(comp_id);
    return 0;
}


void rtapi_app_exit(void) {
    pci_unregister_driver(&hm2_5i20_driver);
    LL_INFO("unloaded driver\n");
    hal_exit(comp_id);
}
