package main

// subscriber_test — P25 Phase 1 ISP packet transmitter for HackRF.
//
// Transmits mobile-originated (ISP) TSBKs on a given frequency using
// a HackRF to stimulate a SiteSim P25 site simulator for testing.
//
// Usage:
//   subscriber_test [flags] <command>
//
// Commands:
//   aff    Group Affiliation Request      (ISP 0x28)
//   vch    Group Voice Channel Request    (ISP 0x00)
//   uu     Unit-to-Unit VCH Request       (ISP 0x03)
//   reg    Unit Registration Request      (ISP 0x2C)
//   dereg  Unit De-Registration           (ISP 0x2F)
//   emrg   Emergency Alarm Request        (ISP 0x0F)
//
// Examples:
//   subscriber_test -freq 145.65 -src 1234567 -tgid 100 aff
//   subscriber_test -freq 145.65 -src 1234567 -sysid 001 reg
//   subscriber_test -freq 145.65 -src 1234567 -tgid 100 -count 48 vch

import (
	"flag"
	"fmt"
	"os"
)

func main() {
	fs := flag.NewFlagSet("subscriber_test", flag.ExitOnError)

	freqMHz  := fs.Float64("freq",   145.65, "TX frequency in MHz")
	srcID    := fs.Int("src",    1234567,  "Source unit ID (24-bit)")
	tgid     := fs.Int("tgid",   100,      "Talkgroup ID (16-bit)")
	targetID := fs.Int("target", 2345678,  "Target unit ID for UU calls")
	nac      := fs.Int("nac",    0x293,    "Network Access Code (12-bit)")
	sysID    := fs.Int("sysid",  0x001,    "System ID for registration requests (12-bit)")
	count    := fs.Int("count",  48,       "Number of frame repetitions")
	gain     := fs.Int("gain",   40,       "TX VGA gain dB (0–47)")
	amp      := fs.Bool("amp",   false,    "Enable HackRF built-in amplifier")
	device   := fs.Int("device", -1,       "HackRF device index to use")
	serial   := fs.String("serial", "",    "HackRF serial number to use")
	listDevs := fs.Bool("list",   false,    "List available HackRF devices and exit")

	fs.Usage = func() {
		fmt.Fprintf(os.Stderr, "P25 ISP packet transmitter for HackRF\n\n")
		fmt.Fprintf(os.Stderr, "Usage: subscriber_test [flags] <command>\n\n")
		fmt.Fprintf(os.Stderr, "Commands:\n")
		fmt.Fprintf(os.Stderr, "  aff    Group Affiliation Request   (ISP 0x28)\n")
		fmt.Fprintf(os.Stderr, "  vch    Group Voice Channel Request (ISP 0x00)\n")
		fmt.Fprintf(os.Stderr, "  uu     Unit-to-Unit VCH Request    (ISP 0x03)\n")
		fmt.Fprintf(os.Stderr, "  reg    Unit Registration Request   (ISP 0x2C)\n")
		fmt.Fprintf(os.Stderr, "  dereg  Unit De-Registration        (ISP 0x2F)\n")
		fmt.Fprintf(os.Stderr, "  emrg   Emergency Alarm Request     (ISP 0x0F)\n\n")
		fmt.Fprintf(os.Stderr, "Flags:\n")
		fs.PrintDefaults()
	}

	if err := fs.Parse(os.Args[1:]); err != nil {
		os.Exit(1)
	}

	if *listDevs {
		devices, err := ListHackRFDevices()
		if err != nil {
			fmt.Fprintf(os.Stderr, "Failed to list HackRF devices: %v\n", err)
			os.Exit(1)
		}
		if len(devices) == 0 {
			fmt.Println("No HackRF devices found.")
			return
		}
		for _, d := range devices {
			fmt.Printf("[%d] %s\n", d.Index, d.Serial)
		}
		return
	}

	cmd := fs.Arg(0)
	if cmd == "" {
		fs.Usage()
		os.Exit(1)
	}
	if *serial != "" && *device >= 0 {
		fmt.Fprintln(os.Stderr, "Use either -serial or -device, not both")
		os.Exit(1)
	}

	freqHz := uint64(*freqMHz * 1e6)
	if *count <= 0 {
		fmt.Fprintln(os.Stderr, "-count must be greater than 0")
		os.Exit(1)
	}

	// ── Build the ISP TSBK ────────────────────────────────────────────────
	var tsbk [12]byte
	var desc string

	switch cmd {
	case "aff":
		tsbk = BuildGrpAffReq(uint16(*tgid), uint32(*srcID))
		desc = fmt.Sprintf("GRP_AFF_REQ  src=%d tgid=%d", *srcID, *tgid)

	case "vch":
		tsbk = BuildGrpVchReq(0x00, uint16(*tgid), uint32(*srcID))
		desc = fmt.Sprintf("GRP_VCH_REQ  src=%d tgid=%d", *srcID, *tgid)

	case "uu":
		tsbk = BuildUuVchReq(0x00, uint32(*targetID), uint32(*srcID))
		desc = fmt.Sprintf("UU_VCH_REQ   src=%d target=%d", *srcID, *targetID)

	case "reg":
		tsbk = BuildURegReq(uint16(*sysID), uint32(*srcID))
		desc = fmt.Sprintf("U_REG_REQ    src=%d sysid=0x%X", *srcID, *sysID)

	case "dereg":
		tsbk = BuildUDeRegAck(uint32(*srcID))
		desc = fmt.Sprintf("U_DE_REG_ACK src=%d", *srcID)

	case "emrg":
		tsbk = BuildEmrgAlrmReq(uint16(*tgid), uint32(*srcID))
		desc = fmt.Sprintf("EMRG_ALRM    src=%d tgid=%d", *srcID, *tgid)

	default:
		fmt.Fprintf(os.Stderr, "Unknown command: %q\n\n", cmd)
		fs.Usage()
		os.Exit(1)
	}

	fmt.Printf("Packet : %s\n", desc)
	fmt.Printf("NAC    : 0x%03X\n", *nac)
	fmt.Printf("SysID  : 0x%03X\n", *sysID)
	fmt.Printf("Freq   : %.4f MHz\n", *freqMHz)
	fmt.Printf("Gain   : %d dB VGA  amp=%v\n", *gain, *amp)
	if *serial != "" {
		fmt.Printf("HackRF : serial=%s\n", *serial)
	} else if *device >= 0 {
		fmt.Printf("HackRF : device=%d\n", *device)
	}
	fmt.Printf("Reps   : %d\n\n", *count)

	fmt.Printf("TSBK bytes: ")
	for i, b := range tsbk {
		fmt.Printf("%02X", b)
		if i < 11 {
			fmt.Printf(" ")
		}
	}
	fmt.Printf("\n\n")

	// ── Modulate ─────────────────────────────────────────────────────────
	fmt.Print("Generating IQ samples... ")
	iq := GenerateIQ(uint16(*nac), tsbk, *count)
	fmt.Printf("done (%d samples, %.1f ms)\n", len(iq)/2,
		float64(len(iq)/2)/sampleRate*1000)

	// ── Open HackRF and transmit ──────────────────────────────────────────
	fmt.Print("Opening HackRF... ")
	hrf, err := OpenHackRF(*serial, *device)
	if err != nil {
		fmt.Fprintf(os.Stderr, "FAIL: %v\n", err)
		os.Exit(1)
	}
	defer hrf.Close()
	fmt.Println("OK")

	fmt.Printf("Transmitting on %.4f MHz...\n", *freqMHz)
	if err := hrf.Transmit(freqHz, sampleRate, uint32(*gain), *amp, iq); err != nil {
		fmt.Fprintf(os.Stderr, "TX error: %v\n", err)
		os.Exit(1)
	}
	fmt.Println("Done.")
}
