"""Execute production voice encoder/parser on Windows without UART hardware.

Uses installed RISC-V GCC and Unicorn emulation. Hardware/RTOS
functions are excluded at the RX-task boundary; protocol functions are copied
verbatim from voice.c, not reimplemented in Python.
"""
import sys
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
GCC = Path(os.environ.get("LAB_HOST_GCC", r"D:\Espressif\tools\riscv32-esp-elf\esp-15.2.0_20251204\riscv32-esp-elf\bin\riscv32-esp-elf-gcc.exe"))
sys.path.insert(0, str(ROOT / "tests/.deps"))

STUBS = r'''
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
typedef int esp_err_t;
typedef struct { int unused; } lab_uart_t;
lab_uart_t lab_voice;
#define ESP_ERR_INVALID_ARG 258
#define pdTRUE 1
#define pdFALSE 0
typedef int BaseType_t;
typedef unsigned TickType_t;
typedef void *QueueHandle_t;
typedef struct {
    uint8_t len, cmd, count;
    uint8_t params[8];
} lab_voice_frame_t;
#define pdMS_TO_TICKS(ms) (ms)
static unsigned good_frames, tx_size;
static uint8_t tx_bytes[256];
static int xQueueSend(QueueHandle_t q, const void *item, TickType_t ticks) {
    (void)q; (void)item; (void)ticks; return pdTRUE;
}
static int xQueueReset(QueueHandle_t q) { (void)q; return pdTRUE; }
static int xQueueReceive(QueueHandle_t q, void *item, TickType_t ticks) {
    (void)q; (void)item; (void)ticks; return pdFALSE;
}
static TickType_t xTaskGetTickCount(void) { return 0; }
void *memcpy(void *d, const void *s, size_t n) {
    unsigned char *dst=d; const unsigned char *src=s;
    for(size_t i=0;i<n;i++) dst[i]=src[i];
    return d;
}
void *memmove(void *d, const void *s, size_t n) {
    unsigned char *dst=d; const unsigned char *src=s;
    if(dst<src) for(size_t i=0;i<n;i++) dst[i]=src[i];
    else for(size_t i=n;i>0;i--) dst[i-1]=src[i-1];
    return d;
}
void *memset(void *d, int c, size_t n) {
    unsigned char *p=d; for(size_t i=0;i<n;i++)p[i]=(unsigned char)c; return d;
}
int printf(const char *fmt, ...) {
    const char prefix[]="[voice] FRAME OK"; unsigned i=0;
    while(prefix[i] && fmt[i]==prefix[i])i++;
    if(!prefix[i])good_frames++;
    return 0;
}
int puts(const char *s) { (void)s; return 0; }
esp_err_t lab_send(lab_uart_t *dev, const uint8_t *b, size_t n) {
    (void)dev;tx_size=(unsigned)n;
    for(size_t i=0;i<n;i++)tx_bytes[i]=b[i];return 0;
}
'''

EXPORTS = r'''
static voice_parser_t parser;
void reset(void) { memset(&parser,0,sizeof(parser));good_frames=0; }
void feed(const uint8_t *b, unsigned n, int64_t now) { parser_feed(&parser,b,n,now); }
void expire(int64_t now) { parser_expire(&parser,now); }
unsigned frames(void) { return good_frames; }
unsigned pending(void) { return (unsigned)parser.used; }
int encode(uint8_t cmd, const uint8_t *b, unsigned n) { return voice_command(cmd,b,n); }
unsigned encoded_size(void) { return tx_size; }
const uint8_t *encoded(void) { return tx_bytes; }
'''


class EmulatedLibrary:
    """Call compiled protocol functions using the RV32 ILP32 ABI."""
    def __init__(self, path):
        from unicorn import Uc, UC_ARCH_RISCV, UC_MODE_RISCV32
        from unicorn import riscv_const as reg
        from elftools.elf.elffile import ELFFile
        self.reg = reg
        self.cpu = Uc(UC_ARCH_RISCV, UC_MODE_RISCV32)
        self.cpu.mem_map(0x1000, 0x400000)
        with path.open("rb") as f:
            elf = ELFFile(f)
            for seg in elf.iter_segments():
                if seg["p_type"] == "PT_LOAD": self.cpu.mem_write(seg["p_vaddr"], seg.data())
            self.symbols = {sym.name: sym["st_value"] for sym in elf.get_section_by_name(".symtab").iter_symbols()}

    def call(self, name, *args):
        words = []
        for arg in args:
            if isinstance(arg, bytes):
                self.cpu.mem_write(0x300000, arg or b"\0")
                words.append(0x300000)
            else: words.append(0 if arg is None else arg)
        # int64_t timestamp uses two argument registers in this RV32 ABI.
        if name in ("feed", "expire"): words.append(0)
        for i, word in enumerate(words): self.cpu.reg_write(self.reg.UC_RISCV_REG_A0+i, word)
        self.cpu.reg_write(self.reg.UC_RISCV_REG_SP, 0x3ff000)
        self.cpu.reg_write(self.reg.UC_RISCV_REG_RA, 0x1000)
        self.cpu.emu_start(self.symbols[name], 0x1000, count=1000000)
        if self.cpu.reg_read(self.reg.UC_RISCV_REG_PC) != 0x1000:
            raise AssertionError("Function did not return within instruction budget")
        return self.cpu.reg_read(self.reg.UC_RISCV_REG_A0)

    def __getattr__(self, name):
        if name == "encoded":
            return lambda: bytes(self.cpu.mem_read(self.call("encoded"), 256))
        return lambda *args: self.call(name, *args)


class VoiceProtocol(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work = tempfile.TemporaryDirectory(prefix="s3_voice_")
        work = Path(cls.work.name)
        config = (ROOT / "main/config.h").read_text(encoding="utf-8")
        defines = "\n".join(re.findall(r"^#define LAB_(?:MAX_SEND_BYTES|VOICE_STALE_MS)\s+.*", config, re.M))
        source = (ROOT / "main/voice.c").read_text(encoding="utf-8").split("static void voice_rx_task", 1)[0]
        source = re.sub(r'^#include .*\n', '', source, flags=re.M)
        c_file, elf = work / "voice_test.c", work / "voice_test.elf"
        c_file.write_text(defines + "\n" + STUBS + source + EXPORTS, encoding="utf-8")
        subprocess.run([str(GCC), "-std=c11", "-march=rv32im", "-mabi=ilp32",
            "-nostdlib", "-ffreestanding", "-fno-builtin", "-fno-stack-protector",
            "-msmall-data-limit=0", "-Wl,--no-relax", "-Wl,--entry=reset",
            "-Wl,-Ttext=0x10000", str(c_file), "-o", str(elf)], check=True)
        cls.lib = EmulatedLibrary(elf)

    @classmethod
    def tearDownClass(cls):
        cls.work.cleanup()

    def setUp(self):
        self.lib.reset()

    def feed(self, data, now=1000):
        self.lib.feed(data, len(data), now)

    def test_encoder_manual_examples(self):
        for cmd, params, expected in [
            (0xc2, b"", "7e03c2c5ef"), (0xc1, b"", "7e03c1c4ef"),
            (0xab, b"", "7e03abaeef"), (0xae, b"\x1f", "7e04ae1fd1ef"),
            (0xa0, b"\x00\x01", "7e05a00001a6ef"),
        ]:
            self.assertEqual(self.lib.encode(cmd, params, len(params)), 0)
            self.assertEqual(bytes(self.lib.encoded()[:self.lib.encoded_size()]).hex(), expected)

    def test_encoder_rejects_oversize_and_null_parameters(self):
        self.assertNotEqual(self.lib.encode(0xa0, None, 1), 0)
        self.assertNotEqual(self.lib.encode(0xa0, b"x" * 252, 252), 0)

    def test_every_split_boundary(self):
        frame = bytes.fromhex("7e04c202c8ef")
        for split in range(1, len(frame)):
            self.lib.reset()
            self.feed(frame[:split])
            self.assertEqual(self.lib.frames(), 0)
            self.feed(frame[split:], 2000)
            self.assertEqual(self.lib.frames(), 1)
            self.assertEqual(self.lib.pending(), 0)

    def test_glued_frames_and_noise(self):
        self.feed(bytes.fromhex("0011227e04c202c8ef7e04c11fe4ef9988"))
        self.assertEqual(self.lib.frames(), 2)
        self.assertEqual(self.lib.pending(), 0)

    def test_bad_checksum_then_valid(self):
        self.feed(bytes.fromhex("7e04c20200ef7e04c202c8ef"))
        self.assertEqual(self.lib.frames(), 1)

    def test_bad_tail_then_valid(self):
        self.feed(bytes.fromhex("7e04c202c8007e04c202c8ef"))
        self.assertEqual(self.lib.frames(), 1)

    def test_illegal_lengths_then_valid(self):
        self.feed(bytes.fromhex("7e007e017e027e04c202c8ef"))
        self.assertEqual(self.lib.frames(), 1)

    def test_embedded_head_and_tail_in_payload(self):
        self.feed(bytes.fromhex("7e05017eef73ef"))
        self.assertEqual(self.lib.frames(), 1)

    def test_corrupt_large_length_recovers_suffix_on_timeout(self):
        self.feed(bytes.fromhex("7eff117e04c202c8ef"))
        self.assertEqual(self.lib.frames(), 0)
        self.lib.expire(600001)
        self.assertEqual(self.lib.frames(), 1)
        self.assertEqual(self.lib.pending(), 0)

    def test_stale_partial_then_new_frame(self):
        self.feed(bytes.fromhex("7e04c2"))
        self.feed(bytes.fromhex("7e04c202c8ef"), 600001)
        self.assertEqual(self.lib.frames(), 1)

    def test_maximum_length_and_bytewise_input(self):
        data = bytes([0x7e, 255, 0x01]) + bytes(252)
        frame = data + bytes([sum(data[1:]) & 255, 0xef])
        self.assertEqual(len(frame), 257)
        for i, byte in enumerate(frame): self.feed(bytes([byte]), 1000 + i)
        self.assertEqual(self.lib.frames(), 1)
        self.assertEqual(self.lib.pending(), 0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
