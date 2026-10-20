#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/kref.h>
#include <linux/uaccess.h>
#include <linux/usb.h>
#include <linux/mutex.h>
#include <linux/kthread.h>

#define MIDI_INTERFACE_CLASS 1
#define MIDI_INTERFACE_SUBCLASS 3
#define MIDI_INTERFACE_PROTOCOL 0

struct midi_device 
{
	struct usb_device *udev;
	struct usb_interface *interface;
    unsigned char *bulk_in_buffer;
	__u16 bulk_in_packet_size;
	__u8 bulk_in_endpointAddr;
	struct kref refCount;
	struct task_struct *urb_test;
};

static const struct usb_device_id midi_table[] = {
	{ USB_INTERFACE_INFO(MIDI_INTERFACE_CLASS, MIDI_INTERFACE_SUBCLASS, MIDI_INTERFACE_PROTOCOL) },
	{ }					/* Terminating entry */
};
MODULE_DEVICE_TABLE(usb, midi_table);

static void midi_delete(struct kref *refCount)
{
	struct midi_device *dev = container_of(refCount, struct midi_device, refCount);
	usb_put_intf(dev->interface);
	usb_put_dev(dev->udev);
	kfree(dev->bulk_in_buffer);
	kfree(dev);
}

/* worker to test urbs and display their content in kernel log */
static int run_test_bulk_in(void *data)
{
	struct midi_device *dev = (struct midi_device *)data;
	int count;
	int retval;
	size_t string_length = dev->bulk_in_packet_size*2+1;
	char *hex_string = kzalloc(string_length, GFP_KERNEL);
	while (!kthread_should_stop())
	{
		retval = usb_bulk_msg(dev->udev, usb_rcvbulkpipe(dev->udev, dev->bulk_in_endpointAddr), dev->bulk_in_buffer, dev->bulk_in_packet_size, &count, HZ);
		if (!retval)
		{
			printk(KERN_DEBUG "Received %d bytes\n", count);
			memset(hex_string, 0, string_length);
			for (size_t i = 0; i < count*2; i+=2)
			{
				unsigned char d = dev->bulk_in_buffer[i/2] >> 4;
				unsigned char r = dev->bulk_in_buffer[i/2] & 0x0f;
				d += '0';
				r += '0';
				if (d > '9') d += 7;
				if (r > '9') r += 7;
				hex_string[i] = d;
				hex_string[i+1] = r;
			}
			hex_string[count*2] = '\0';
			printk(KERN_DEBUG "%s\n", hex_string);
		}
		else
		{
			if (retval != -ETIMEDOUT)
        		printk(KERN_DEBUG "usb_bulk_msg failed %d\n", retval);
		}
	}
	kfree(hex_string);

	return 0;
}

static int midi_probe(struct usb_interface *interface, const struct usb_device_id *id)
{
	printk(KERN_DEBUG "MIDI probe function called\n");
	struct midi_device *dev;
	int retval;
	int endpoints_found = 0;
	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;
	kref_init(&dev->refCount);
	dev->udev = usb_get_dev(interface_to_usbdev(interface));
	dev->interface = usb_get_intf(interface);
	for (size_t i = 0; i < interface->cur_altsetting->desc.bNumEndpoints; ++i)
	{
		const struct usb_endpoint_descriptor *endpoint = &interface->cur_altsetting->endpoint[i].desc;
		if (((endpoint->bmAttributes & 0x03) == 0x02) && (endpoint->bEndpointAddress & 0x80))
		{
			endpoints_found++;
			dev->bulk_in_endpointAddr = endpoint->bEndpointAddress;
			dev->bulk_in_packet_size = le16_to_cpu(endpoint->wMaxPacketSize);
			dev->bulk_in_buffer = kmalloc(dev->bulk_in_packet_size, GFP_KERNEL);
			if (!dev->bulk_in_buffer)
			{
				retval = -ENOMEM;
				goto error;
			}
		}
	}
	if (!endpoints_found)
	{
		retval = -ENODEV;
		goto error;
	}
	printk(KERN_DEBUG "allocated %dB for bulk in transactions at endpoint addr 0x%02x (IN)\n", dev->bulk_in_packet_size, dev->bulk_in_endpointAddr);
	usb_set_intfdata(interface, dev);
	printk(KERN_DEBUG "Device connected: %s %s\n", dev->udev->manufacturer, dev->udev->product);
	dev->urb_test = kthread_run(run_test_bulk_in, dev, "test_bulk");
	if (IS_ERR(dev->urb_test))
	{
		printk("KERN_ERR Couldn't run urb test task\n");
		usb_set_intfdata(interface, NULL);
		retval = -ENODEV;
		goto error;
	}
	
	return 0;

error:
	kref_put(&dev->refCount, midi_delete);

	return retval;
}

static void midi_disconnect(struct usb_interface *interface)
{
	printk(KERN_DEBUG "MIDI disconnect function called\n");
	struct midi_device *dev;
	dev = usb_get_intfdata(interface);
	if (dev->urb_test)
	{
		kthread_stop(dev->urb_test);
		printk(KERN_DEBUG "Stopped urb test task\n");
	}
	printk(KERN_DEBUG "Device disconnected: %s %s\n", dev->udev->manufacturer, dev->udev->product);
	kref_put(&dev->refCount, midi_delete);
}

static struct usb_driver midi_driver = {
	.name =	"midi",
	.id_table =	midi_table,
	.probe = midi_probe,
	.disconnect = midi_disconnect,
};

module_usb_driver(midi_driver); /* instead of init and exit */

MODULE_LICENSE("GPL");