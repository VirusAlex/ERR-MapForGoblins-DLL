import struct, os, sys

def dump_dcx(path, label):
    data = open(path, 'rb').read()
    print(f"\n=== {label} === ({len(data)} bytes)")
    print(f"  Magic: {data[:4]}")
    
    # DCX header (big-endian)
    print(f"  Header hex [0x00-0x4F]:")
    for row in range(0, min(0x50, len(data)), 16):
        hex_str = ' '.join(f'{data[row+i]:02X}' for i in range(min(16, len(data)-row)))
        print(f"    {row:04X}: {hex_str}")
    
    if data[:4] == b'DCX\x00':
        unk04 = struct.unpack('>I', data[0x04:0x08])[0]
        dcs_offset = struct.unpack('>I', data[0x08:0x0C])[0]
        dcp_offset = struct.unpack('>I', data[0x0C:0x10])[0]
        unk10 = struct.unpack('>I', data[0x10:0x14])[0]
        unk14 = struct.unpack('>I', data[0x14:0x18])[0]
        
        dcs_magic = data[0x18:0x1C]
        uncomp_size = struct.unpack('>I', data[0x1C:0x20])[0]
        comp_size = struct.unpack('>I', data[0x20:0x24])[0]
        
        dcp_magic = data[0x24:0x28]
        dcp_method = data[0x28:0x2C]
        
        unk2C = struct.unpack('>I', data[0x2C:0x30])[0]
        unk30 = struct.unpack('>I', data[0x30:0x34])[0]
        unk34 = struct.unpack('>I', data[0x34:0x38])[0]
        unk38 = struct.unpack('>I', data[0x38:0x3C])[0]
        unk3C = struct.unpack('>I', data[0x3C:0x40])[0]
        unk40 = struct.unpack('>I', data[0x40:0x44])[0]
        
        dca_magic = data[0x44:0x48]
        dca_size = struct.unpack('>I', data[0x48:0x4C])[0]
        
        print(f"\n  unk04=0x{unk04:08X}, dcs_off=0x{dcs_offset:08X}, dcp_off=0x{dcp_offset:08X}")
        print(f"  unk10=0x{unk10:08X}, unk14=0x{unk14:08X}")
        print(f"  DCS magic={dcs_magic}, uncomp_size={uncomp_size}, comp_size={comp_size}")
        print(f"  DCP magic={dcp_magic}, method={dcp_method}")
        print(f"  unk2C=0x{unk2C:08X}, unk30=0x{unk30:08X}, unk34=0x{unk34:08X}")
        print(f"  unk38=0x{unk38:08X}, unk3C=0x{unk3C:08X}, unk40=0x{unk40:08X}")
        print(f"  DCA magic={dca_magic}, dca_size=0x{dca_size:08X}")
        print(f"  Compressed data starts at 0x4C, actual data size={len(data)-0x4C}")
        print(f"  comp_size matches data? {comp_size == len(data)-0x4C}")

if len(sys.argv) < 2:
    print("Usage: compare_dcx.py <file1.dcx> [file2.dcx ...]")
    print("Dumps and compares DCX header structure of one or more files.")
    sys.exit(1)

for path in sys.argv[1:]:
    dump_dcx(path, os.path.basename(path))
