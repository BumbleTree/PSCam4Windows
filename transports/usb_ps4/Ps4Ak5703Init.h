#pragma once
//
// Ps4Ak5703Init.h — the OV580's on-board AK5703 microphone ADC bring-up.
//
// GENERATED from bigboss-ps3dev/PS4EYECam driver/include/ps4eye_regs.h
// (ak5703_reg_init0..9, written in that order, all at subaddress 0x24).
//
// WHY THIS EXISTS. The PS4 camera has no USB-audio function: its 4-mic array is
// delivered inside the video stream, 64 bytes in every row between the 32-byte
// row header and the left eye band. Those 64 bytes read as ALL ZEROS until the
// ADC is powered up and configured — which is what this table does. Without it
// the microphone looks like a hardware wall; with it the field carries live
// 4-channel audio (byte activity across the field goes 0/64 -> 64/64).
//
// Two details make this unreachable by accident:
//   * the vendor register payload is [0]=mode, [1..7]=0, then 4-byte records
//     {reg_lo, reg_hi, val, subaddr} from offset 8, length 8+4n — NOT the raw
//     buffer offsets the reference driver uses, which include libusb'''s 8-byte
//     control SETUP header;
//   * the mode byte is per-subaddress: 0x03 for the sensors and the OV580,
//     but 0x0b to write the AK5703 (0x08 to read it).
//
namespace ps4 {

static const unsigned char kAk5703Subaddr   = 0x24;
static const unsigned char kAk5703WriteMode = 0x0b;
static const unsigned char kAk5703ReadMode  = 0x08;

struct Ak5703Reg { unsigned short reg; unsigned char val; };
static const Ak5703Reg kAk5703Init[] = {
    // ak5703_reg_init0
    { 0x0000, 0x0f },
    // ak5703_reg_init1
    { 0x0000, 0x00 },
    { 0x0010, 0x00 },
    // ak5703_reg_init2
    { 0x0001, 0x09 },
    { 0x0002, 0xc0 },
    { 0x0003, 0xc4 },
    { 0x0004, 0x04 },
    { 0x0005, 0x0b },
    { 0x0006, 0x00 },
    { 0x0007, 0xa8 },
    { 0x0008, 0xa8 },
    { 0x0009, 0x84 },
    { 0x000a, 0x00 },
    { 0x000b, 0xe1 },
    { 0x000c, 0x00 },
    // ak5703_reg_init3
    { 0x000d, 0x00 },
    { 0x000e, 0x00 },
    { 0x000f, 0x00 },
    { 0x0011, 0x00 },
    { 0x0012, 0xc0 },
    { 0x0013, 0x04 },
    { 0x0014, 0x04 },
    { 0x0015, 0x00 },
    { 0x0016, 0x00 },
    { 0x0017, 0xa8 },
    { 0x0018, 0xa8 },
    { 0x0019, 0x80 },
    // ak5703_reg_init4
    { 0x001a, 0x00 },
    { 0x001b, 0xe1 },
    { 0x001c, 0x00 },
    { 0x001d, 0x00 },
    { 0x001e, 0x00 },
    { 0x001f, 0x00 },
    { 0x0020, 0xa9 },
    { 0x0021, 0x1f },
    { 0x0022, 0xad },
    { 0x0023, 0x20 },
    { 0x0024, 0x00 },
    { 0x0025, 0x00 },
    // ak5703_reg_init5
    { 0x0026, 0x00 },
    { 0x0027, 0x00 },
    { 0x0028, 0x00 },
    { 0x0028, 0x00 },
    { 0x002a, 0x00 },
    { 0x002b, 0x00 },
    { 0x002c, 0x00 },
    { 0x002d, 0x00 },
    { 0x002e, 0xad },
    { 0x002f, 0x00 },
    { 0x0030, 0x09 },
    { 0x0031, 0x1f },
    // ak5703_reg_init6
    { 0x0032, 0x0d },
    { 0x0033, 0x20 },
    { 0x0034, 0x00 },
    { 0x0035, 0x00 },
    { 0x0036, 0x00 },
    { 0x0037, 0x00 },
    { 0x0001, 0x08 },
    { 0x0003, 0xc4 },
    { 0x0005, 0x0b },
    { 0x0000, 0x04 },
    // ak5703_reg_init7
    { 0x0001, 0x09 },
    // ak5703_reg_init8
    { 0x0000, 0x0c },
    { 0x0010, 0x08 },
    // ak5703_reg_init9
    { 0x0002, 0xc0 },
    { 0x0007, 0xa8 },
    { 0x0008, 0xa8 },
    { 0x0012, 0xc0 },
    { 0x0017, 0xa8 },
    { 0x0018, 0xa8 },
    { 0x0000, 0x0f },
    { 0x0010, 0x0b },
};
static const int kAk5703InitCount = (int)(sizeof(kAk5703Init) / sizeof(kAk5703Init[0]));

// Registers the reference driver reads back after the init (Sony's own driver
// does the same in the USB capture), useful as an "is the ADC answering" probe.

} // namespace ps4
