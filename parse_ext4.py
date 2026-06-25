#!/usr/bin/env python3
"""
Ext4 filesystem parser - reads ext4 image directly without mounting.
"""

import struct
import sys

def read_le(data, offset, fmt):
    return struct.unpack_from(fmt, data, offset)[0]


class Ext4Parser:
    S_IFMT = 0xF000
    S_IFDIR = 0x4000
    S_IFREG = 0x8000
    S_IFLNK = 0xA000
    EXT4_EXTENTS_FL = 0x80000
    
    def __init__(self, image_path):
        self.f = open(image_path, 'rb')
        self._read_superblock()
        self._print_sb_info()
    
    def _read_superblock(self):
        self.f.seek(1024)
        data = self.f.read(1024)
        
        sb = {}
        sb['inodes_count'] = read_le(data, 0, '<I')
        sb['blocks_count'] = read_le(data, 4, '<I')
        sb['first_data_block'] = read_le(data, 0x14, '<I')
        log_block_size = read_le(data, 0x18, '<I')
        sb['block_size'] = 1024 << log_block_size
        sb['blocks_per_group'] = read_le(data, 0x20, '<I')
        sb['inodes_per_group'] = read_le(data, 0x28, '<I')
        sb['first_ino'] = read_le(data, 0x54, '<I')
        sb['inode_size'] = read_le(data, 0x58, '<H')
        sb['desc_size'] = read_le(data, 0xFE, '<H')
        if sb['desc_size'] == 0:
            sb['desc_size'] = 32
        self.sb = sb
    
    def _print_sb_info(self):
        sb = self.sb
        print("=== Superblock Info ===")
        print(f"  Inodes count:     {sb['inodes_count']}")
        print(f"  Blocks count:     {sb['blocks_count']}")
        print(f"  Block size:       {sb['block_size']}")
        print(f"  Blocks per group: {sb['blocks_per_group']}")
        print(f"  Inodes per group: {sb['inodes_per_group']}")
        print(f"  Inode size:       {sb['inode_size']}")
        print(f"  First inode:      {sb['first_ino']}")
        print(f"  Desc size:        {sb['desc_size']}")
        num_groups = (sb['blocks_count'] + sb['blocks_per_group'] - 1) // sb['blocks_per_group']
        print(f"  Block groups:     {num_groups}")
        print()
    
    def _get_bg_and_index(self, inode_no):
        bg = (inode_no - 1) // self.sb['inodes_per_group']
        idx = (inode_no - 1) % self.sb['inodes_per_group']
        return bg, idx
    
    def _read_bgd(self, bg_num):
        desc_size = self.sb['desc_size']
        bgd_offset = self.sb['block_size']
        
        self.f.seek(bgd_offset + bg_num * desc_size)
        data = self.f.read(desc_size)
        
        bgd = {}
        bgd['block_bitmap'] = read_le(data, 0, '<I')
        bgd['inode_bitmap'] = read_le(data, 4, '<I')
        bgd['inode_table'] = read_le(data, 8, '<I')
        
        if desc_size >= 64:
            bgd['inode_table_hi'] = read_le(data, 40, '<I')
            bgd['inode_table'] |= bgd['inode_table_hi'] << 32
        
        return bgd
    
    def _read_inode_raw(self, inode_no):
        bg_num, idx = self._get_bg_and_index(inode_no)
        bgd = self._read_bgd(bg_num)
        
        inode_table_block = bgd['inode_table']
        inode_size = self.sb['inode_size']
        block_size = self.sb['block_size']
        
        offset = inode_table_block * block_size + idx * inode_size
        self.f.seek(offset)
        return self.f.read(inode_size)
    
    def parse_inode(self, inode_no):
        data = self._read_inode_raw(inode_no)
        
        inode = {}
        inode['mode'] = read_le(data, 0, '<H')
        inode['size_lo'] = read_le(data, 4, '<I')
        inode['blocks_lo'] = read_le(data, 28, '<I')
        inode['flags'] = read_le(data, 32, '<I')
        inode['block'] = list(struct.unpack_from('<15I', data, 40))
        inode['size_high'] = read_le(data, 108, '<I')
        inode['size'] = inode['size_lo'] | (inode['size_high'] << 32)
        
        return inode
    
    def type_str(self, inode):
        mode = inode['mode'] & 0xF000
        if mode == self.S_IFDIR:
            return 'directory'
        elif mode == self.S_IFREG:
            return 'file'
        elif mode == self.S_IFLNK:
            return 'symlink'
        else:
            return f'0x{mode:04x}'
    
    def is_type(self, inode, mask):
        return (inode['mode'] & self.S_IFMT) == mask
    
    def _read_extent_node(self, data, offset, depth):
        eh_magic = read_le(data, offset, '<H')
        if eh_magic != 0xF30A:
            return []
        
        eh_entries = read_le(data, offset + 2, '<H')
        
        blocks = []
        for i in range(eh_entries):
            entry_off = offset + 12 + i * 12
            
            if depth == 0:
                ee_block = read_le(data, entry_off, '<I')
                ee_len   = read_le(data, entry_off + 4, '<H')
                ee_start_hi = read_le(data, entry_off + 6, '<H')
                ee_start_lo = read_le(data, entry_off + 8, '<I')
                ee_start = (ee_start_hi << 32) | ee_start_lo
                
                if ee_len > 0x8000:
                    ee_len = 0x10000 - ee_len
                
                for b in range(ee_len):
                    blocks.append(ee_start + b)
            else:
                ei_block   = read_le(data, entry_off, '<I')
                ei_leaf_lo = read_le(data, entry_off + 4, '<I')
                ei_leaf_hi = read_le(data, entry_off + 8, '<H')
                leaf_block = (ei_leaf_hi << 32) | ei_leaf_lo
                
                child_offset = leaf_block * self.sb['block_size']
                self.f.seek(child_offset)
                child_data = self.f.read(self.sb['block_size'])
                
                child_blocks = self._read_extent_node(child_data, 0, depth - 1)
                blocks.extend(child_blocks)
        
        return blocks
    
    def read_inode_data(self, inode):
        size = inode['size']
        
        # Fast symlink: data stored inline in i_block (up to 60 bytes)
        if self.is_type(inode, self.S_IFLNK) and size <= 60:
            return struct.pack('<15I', *inode['block'])[:size]
        
        if size == 0:
            return b''
        
        # Extent-mapped files
        if inode['flags'] & self.EXT4_EXTENTS_FL:
            block_data = struct.pack('<15I', *inode['block'])
            eh_magic = read_le(block_data, 0, '<H')
            if eh_magic == 0xF30A:
                eh_depth = read_le(block_data, 6, '<H')
                blocks = self._read_extent_node(block_data, 0, eh_depth)
            else:
                blocks = []
        else:
            blocks = []
        
        # Fallback: direct/indirect blocks from i_block
        if not blocks:
            bs = self.sb['block_size']
            for b in inode['block'][:12]:  # direct blocks
                if b:
                    blocks.append(b)
        
        data = b''
        bs = self.sb['block_size']
        for block_num in blocks:
            self.f.seek(block_num * bs)
            chunk = self.f.read(bs)
            data += chunk
            if len(data) >= size:
                break
        
        return data[:size]
    
    def read_directory(self, inode_no):
        inode = self.parse_inode(inode_no)
        data = self.read_inode_data(inode)
        
        entries = []
        offset = 0
        while offset < len(data):
            if offset + 8 > len(data):
                break
            
            inum = read_le(data, offset, '<I')
            rec_len = read_le(data, offset + 4, '<H')
            name_len = data[offset + 6]
            file_type = data[offset + 7]
            
            if inum == 0 or rec_len == 0:
                offset += rec_len if rec_len > 0 else 8
                continue
            
            name = data[offset + 8:offset + 8 + name_len].decode('utf-8', errors='replace')
            
            entries.append({
                'inode': inum,
                'name': name,
                'file_type': file_type,
            })
            
            offset += rec_len
        
        return entries
    
    def find_inode_by_path(self, path):
        parts = path.strip('/').split('/')
        if not parts or parts == ['']:
            return 2
        
        current = 2
        for part in parts:
            entries = self.read_directory(current)
            found = False
            for entry in entries:
                if entry['name'] == part:
                    current = entry['inode']
                    found = True
                    break
            if not found:
                return None
        
        return current
    
    def read_file_data(self, path):
        inode_no = self.find_inode_by_path(path)
        if inode_no is None:
            return None, None
        
        inode = self.parse_inode(inode_no)
        data = self.read_inode_data(inode)
        return data, inode
    
    def close(self):
        self.f.close()


def print_listing(title, entries, parser):
    print(f"=== {title} ===")
    for e in entries:
        inode = parser.parse_inode(e['inode'])
        t = parser.type_str(inode)
        sz = inode['size']
        ftype_desc = ['unknown', 'regular file', 'directory', 'character device',
                      'block device', 'fifo', 'socket', 'symlink'][e['file_type']] if e['file_type'] < 8 else '?'
        print(f"  {e['name']:30} inode={e['inode']:>6}  type={t:10} size={sz:>8}  ft={ftype_desc}")
    print()


def main():
    img_path = '/home/ut006457@uos/ow/Yu-NoobKernel/sdcard-rv.img'
    parser = Ext4Parser(img_path)
    
    # 1. Root directory
    root_entries = parser.read_directory(2)
    print_listing("Root Directory (/)", root_entries, parser)
    
    # 2. /musl/ directory
    musl_inode = parser.find_inode_by_path('/musl')
    if musl_inode:
        musl_entries = parser.read_directory(musl_inode)
        print_listing("/musl/ Directory", musl_entries, parser)
    else:
        print("=== /musl/ Directory ===")
        print("  NOT FOUND\n")
        musl_entries = []
    
    # 3. Check specific files
    for fname in ['/musl/busybox_testcode.sh', '/musl/busybox_cmd.txt']:
        print(f"=== {fname} ===")
        data, inode = parser.read_file_data(fname)
        if data is not None:
            print(f"  Size: {len(data)} bytes")
            print(f"  Content:")
            print(data.decode('utf-8', errors='replace'))
        else:
            print("  NOT FOUND")
        print()
    
    # 4. Check bin directories
    print("=== Bin directory check ===")
    for d in ['/bin', '/usr/bin', '/usr/local/bin', '/usr']:
        inode_no = parser.find_inode_by_path(d)
        if inode_no:
            inode = parser.parse_inode(inode_no)
            if parser.is_type(inode, Ext4Parser.S_IFDIR):
                print(f"  {d}: EXISTS (directory)")
            else:
                print(f"  {d}: EXISTS but is {parser.type_str(inode)}")
        else:
            print(f"  {d}: NOT FOUND")
    print()
    
    # 5. Symlinks
    print("=== Symlinks in / ===")
    for e in root_entries:
        inode = parser.parse_inode(e['inode'])
        if parser.is_type(inode, Ext4Parser.S_IFLNK):
            target = parser.read_inode_data(inode).decode('utf-8', errors='replace').rstrip('\x00')
            print(f"  {e['name']} -> {target}")
    
    if musl_entries:
        print()
        print("=== Symlinks in /musl/ ===")
        for e in musl_entries:
            inode = parser.parse_inode(e['inode'])
            if parser.is_type(inode, Ext4Parser.S_IFLNK):
                target = parser.read_inode_data(inode).decode('utf-8', errors='replace').rstrip('\x00')
                print(f"  {e['name']} -> {target}")
    
    # 5b. Common commands check in /bin /usr/bin etc.
    common_cmds = ['cat', 'echo', 'ls', 'sh', 'bash', 'cp', 'mv', 'rm', 'grep', 'find', 'sed', 'awk']
    for d in ['/bin', '/usr/bin', '/usr/local/bin', '/sbin', '/usr/sbin']:
        dinode = parser.find_inode_by_path(d)
        if dinode:
            print(f"\n=== Contents of {d} ===")
            entries = parser.read_directory(dinode)
            for e in entries:
                inode = parser.parse_inode(e['inode'])
                t = parser.type_str(inode)
                sz = inode['size']
                if parser.is_type(inode, Ext4Parser.S_IFLNK):
                    target = parser.read_inode_data(inode).decode('utf-8', errors='replace').rstrip('\x00')
                    print(f"  {e['name']:20} -> {target}")
                elif e['name'] in common_cmds:
                    print(f"  {e['name']:20} {t:10} size={sz}")
                elif e['file_type'] == 2:  # directory
                    print(f"  {e['name']:20} subdirectory")
    
    parser.close()


if __name__ == '__main__':
    main()
