package main

/*
#cgo LDFLAGS: -lhackrf
#include <libhackrf/hackrf.h>
#include <stdlib.h>
#include <string.h>

// Global TX state — single-threaded use only.
static uint8_t* g_tx_buf  = NULL;
static int      g_tx_len  = 0;
static int      g_tx_pos  = 0;
static volatile int g_tx_done = 0;

static int tx_callback(hackrf_transfer* transfer) {
    int remaining = g_tx_len - g_tx_pos;
    if (remaining <= 0) {
        memset(transfer->buffer, 0, transfer->valid_length);
        g_tx_done = 1;
        return -1;
    }
    int tocopy = remaining < transfer->valid_length ? remaining : transfer->valid_length;
    memcpy(transfer->buffer, g_tx_buf + g_tx_pos, tocopy);
    if (tocopy < transfer->valid_length)
        memset(transfer->buffer + tocopy, 0, transfer->valid_length - tocopy);
    g_tx_pos += tocopy;
    if (g_tx_pos >= g_tx_len)
        g_tx_done = 1;
    return 0;
}

static int start_tx(hackrf_device* dev, uint8_t* buf, int len) {
    g_tx_buf  = buf;
    g_tx_len  = len;
    g_tx_pos  = 0;
    g_tx_done = 0;
    return hackrf_start_tx(dev, tx_callback, NULL);
}

static int tx_done() { return g_tx_done; }
*/
import "C"

import (
	"fmt"
	"time"
	"unsafe"
)

// HackRF wraps a libhackrf device handle.
type HackRF struct {
	dev *C.hackrf_device
}

type HackRFDeviceInfo struct {
	Index  int
	Serial string
}

func ListHackRFDevices() ([]HackRFDeviceInfo, error) {
	if ret := C.hackrf_init(); ret != C.HACKRF_SUCCESS {
		return nil, fmt.Errorf("hackrf_init: code %d", int(ret))
	}
	defer C.hackrf_exit()

	list := C.hackrf_device_list()
	if list == nil {
		return nil, fmt.Errorf("hackrf_device_list returned nil")
	}
	defer C.hackrf_device_list_free(list)

	count := int(list.devicecount)
	devices := make([]HackRFDeviceInfo, 0, count)
	serials := unsafe.Slice(list.serial_numbers, count)
	for i := 0; i < count; i++ {
		serial := ""
		if serials[i] != nil {
			serial = C.GoString(serials[i])
		}
		devices = append(devices, HackRFDeviceInfo{
			Index:  i,
			Serial: serial,
		})
	}
	return devices, nil
}

// OpenHackRF initialises libhackrf and opens the requested device.
// serial takes priority over index. Use index < 0 to select the default device.
func OpenHackRF(serial string, index int) (*HackRF, error) {
	if ret := C.hackrf_init(); ret != C.HACKRF_SUCCESS {
		return nil, fmt.Errorf("hackrf_init: code %d", int(ret))
	}

	var dev *C.hackrf_device

	switch {
	case serial != "":
		cserial := C.CString(serial)
		defer C.free(unsafe.Pointer(cserial))
		if ret := C.hackrf_open_by_serial(cserial, &dev); ret != C.HACKRF_SUCCESS {
			C.hackrf_exit()
			return nil, fmt.Errorf("hackrf_open_by_serial(%q): code %d", serial, int(ret))
		}
	case index >= 0:
		list := C.hackrf_device_list()
		if list == nil {
			C.hackrf_exit()
			return nil, fmt.Errorf("hackrf_device_list returned nil")
		}
		defer C.hackrf_device_list_free(list)
		if index >= int(list.devicecount) {
			C.hackrf_exit()
			return nil, fmt.Errorf("device index %d out of range (found %d device(s))", index, int(list.devicecount))
		}
		if ret := C.hackrf_device_list_open(list, C.int(index), &dev); ret != C.HACKRF_SUCCESS {
			C.hackrf_exit()
			return nil, fmt.Errorf("hackrf_device_list_open(%d): code %d", index, int(ret))
		}
	default:
		if ret := C.hackrf_open(&dev); ret != C.HACKRF_SUCCESS {
			C.hackrf_exit()
			return nil, fmt.Errorf("hackrf_open: code %d — is HackRF connected?", int(ret))
		}
	}

	return &HackRF{dev: dev}, nil
}

// Close stops TX (if running), closes the device and exits libhackrf.
func (h *HackRF) Close() {
	C.hackrf_stop_tx(h.dev)
	C.hackrf_close(h.dev)
	C.hackrf_exit()
}

// Transmit sends iqData (interleaved int8 I/Q) at freqHz.
// gain is TX VGA gain in dB (0–47).  ampEnable enables the built-in amp.
func (h *HackRF) Transmit(freqHz uint64, sampleRateHz float64, gain uint32, ampEnable bool, iqData []int8) error {
	check := func(label string, ret C.int) error {
		if ret != C.HACKRF_SUCCESS {
			return fmt.Errorf("%s: code %d", label, int(ret))
		}
		return nil
	}

	if err := check("set_freq", C.hackrf_set_freq(h.dev, C.uint64_t(freqHz))); err != nil {
		return err
	}
	if err := check("set_sample_rate", C.hackrf_set_sample_rate(h.dev, C.double(sampleRateHz))); err != nil {
		return err
	}
	if err := check("set_txvga_gain", C.hackrf_set_txvga_gain(h.dev, C.uint32_t(gain))); err != nil {
		return err
	}
	var amp C.uint8_t
	if ampEnable {
		amp = 1
	}
	if err := check("set_amp_enable", C.hackrf_set_amp_enable(h.dev, amp)); err != nil {
		return err
	}

	// Copy IQ data to C-managed heap so the TX callback can access it safely.
	buf := C.malloc(C.size_t(len(iqData)))
	if buf == nil {
		return fmt.Errorf("malloc failed")
	}
	defer C.free(buf)
	C.memcpy(buf, unsafe.Pointer(&iqData[0]), C.size_t(len(iqData)))

	if ret := C.start_tx(h.dev, (*C.uint8_t)(buf), C.int(len(iqData))); ret != C.HACKRF_SUCCESS {
		return fmt.Errorf("hackrf_start_tx: code %d", int(ret))
	}

	expected := time.Duration(float64(len(iqData)/2)/sampleRateHz*float64(time.Second)) + 2*time.Second
	deadline := time.Now().Add(expected)
	for C.tx_done() == 0 {
		if time.Now().After(deadline) {
			C.hackrf_stop_tx(h.dev)
			return fmt.Errorf("timed out waiting for TX completion")
		}
		time.Sleep(10 * time.Millisecond)
	}

	C.hackrf_stop_tx(h.dev)
	return nil
}
