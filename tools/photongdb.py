#!/usr/bin/python
# -*- coding: utf-8 -*-
#
# Copyright 2022 The Photon Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
"""
PhotonLibOS GDB Extension v2
============================
Unified coroutine debugging tool, supports live process and coredump.
Uses manual stack unwinding without modifying registers.

Commands:
  photon_ls    - List all coroutines
  photon_bt    - Show backtrace for specified coroutine
  photon_ps    - Show backtrace for all coroutines
"""

import gdb

# =============================================================================
# Color output
# =============================================================================

class bcolors:
    HEADER = '\033[95m'
    OKBLUE = '\033[94m'
    OKCYAN = '\033[96m'
    OKGREEN = '\033[92m'
    WARNING = '\033[93m'
    FAIL = '\033[91m'
    ENDC = '\033[0m'
    BOLD = '\033[1m'
    UNDERLINE = '\033[4m'

CMAP = {
    'CURRENT': bcolors.OKGREEN,
    'READY': bcolors.OKCYAN,
    'SLEEP': bcolors.OKBLUE,
    'STANDBY': bcolors.WARNING,
    'WARNING': bcolors.HEADER,
    'INFO': bcolors.OKGREEN,
    'ERROR': bcolors.FAIL,
}

def cprint(stat, *args):
    color = CMAP.get(stat, '')
    print('{}{}{} {}'.format(color, stat, bcolors.ENDC,
                             ' '.join(str(x) for x in args)))

# =============================================================================
# Current thread selection (like GDB's current thread)
# =============================================================================

# Global state: currently selected photon thread index
_selected_thread_idx = 0

# =============================================================================
# Memory read utilities (no function calls)
# =============================================================================

def read_ptr(addr):
    """Read pointer value"""
    try:
        return int(gdb.parse_and_eval(f"*(unsigned long*){addr}"))
    except gdb.MemoryError:
        return 0
    except:
        return 0

def read_u64(addr):
    """Read uint64_t"""
    return read_ptr(addr)

def read_u32(addr):
    """Read uint32_t"""
    try:
        return int(gdb.parse_and_eval(f"*(unsigned int*){addr}"))
    except:
        return 0

def read_u16(addr):
    """Read uint16_t"""
    try:
        return int(gdb.parse_and_eval(f"*(unsigned short*){addr}"))
    except:
        return 0

# =============================================================================
# Architecture detection
# =============================================================================

def get_arch():
    """Get current architecture"""
    try:
        frame = gdb.selected_frame()
        arch = frame.architecture()
        return arch.name()
    except:
        # Try to get from inferior
        try:
            return gdb.selected_inferior().architecture().name()
        except:
            return 'i386:x86-64'  # Default

def is_aarch64():
    return 'aarch64' in get_arch()

# =============================================================================
# Data structure offsets - loaded from symbols at runtime
# Offsets are exported as global symbols in libphoton.so (gdb_thread_offset_*)
# =============================================================================

# Default offsets (fallback if symbols not available)
_DEFAULT_THREAD_OFFSETS = {
    '_size': 120,
    'prev': 0,
    'next': 8,
    'vcpu': 16,
    'stack_ptr': 24,
    'idx': 32,
    'error_number': 36,
    'waitq': 40,
    'flags': 48,
    'state': 52,
    'ts_wakeup': 56,
    'tls': 64,
    'buf': 80,
    'stack_size': 96,
}

_DEFAULT_VCPU_OFFSETS = {
    '_size': 200,
    'sleepq': 16,
    'nthreads': 44,
    'idle_worker': 48,
    'standbyq': 56,
    'list_node_prev': 72,
    'list_node_next': 80,
}

STATE_NAMES = {
    0: 'READY',
    1: 'RUNNING',
    2: 'SLEEPING',
    4: 'DONE',
    8: 'STANDBY',
}

def _read_gdb_symbol(name):
    """Read a global symbol value from GDB."""
    try:
        return int(gdb.parse_and_eval(name))
    except:
        return None

def _load_offsets_from_symbols():
    """
    Load offsets from exported symbols in libphoton.so.
    Returns (thread_offsets, vcpu_offsets, success).
    """
    thread_offsets = {}
    vcpu_offsets = {}
    
    # Thread offsets
    thread_symbols = [
        ('_size', 'gdb_thread_size'),
        ('prev', 'gdb_thread_offset_prev'),
        ('next', 'gdb_thread_offset_next'),
        ('vcpu', 'gdb_thread_offset_vcpu'),
        ('stack_ptr', 'gdb_thread_offset_stack_ptr'),
        ('idx', 'gdb_thread_offset_idx'),
        ('error_number', 'gdb_thread_offset_error_number'),
        ('waitq', 'gdb_thread_offset_waitq'),
        ('flags', 'gdb_thread_offset_flags'),
        ('state', 'gdb_thread_offset_state'),
        ('ts_wakeup', 'gdb_thread_offset_ts_wakeup'),
        ('tls', 'gdb_thread_offset_tls'),
        ('buf', 'gdb_thread_offset_buf'),
        ('stack_size', 'gdb_thread_offset_stack_size'),
    ]
    
    # vCPU offsets
    vcpu_symbols = [
        ('_size', 'gdb_vcpu_size'),
        ('sleepq', 'gdb_vcpu_offset_sleepq'),
        ('nthreads', 'gdb_vcpu_offset_nthreads'),
        ('idle_worker', 'gdb_vcpu_offset_idle_worker'),
        ('standbyq', 'gdb_vcpu_offset_standbyq'),
        ('list_node_prev', 'gdb_vcpu_offset_list_node_prev'),
        ('list_node_next', 'gdb_vcpu_offset_list_node_next'),
    ]
    
    success = True
    
    for name, symbol in thread_symbols:
        val = _read_gdb_symbol(symbol)
        if val is not None:
            thread_offsets[name] = val
        else:
            thread_offsets[name] = _DEFAULT_THREAD_OFFSETS.get(name, 0)
            success = False
    
    for name, symbol in vcpu_symbols:
        val = _read_gdb_symbol(symbol)
        if val is not None:
            vcpu_offsets[name] = val
        else:
            vcpu_offsets[name] = _DEFAULT_VCPU_OFFSETS.get(name, 0)
            success = False
    
    return thread_offsets, vcpu_offsets, success

# Initialize offsets - will be loaded when first needed
THREAD_OFFSETS = None
VCPU_OFFSETS = None
_offsets_loaded = False
_offsets_from_symbols = False

def _ensure_offsets_loaded():
    """Lazy load offsets from symbols."""
    global THREAD_OFFSETS, VCPU_OFFSETS, _offsets_loaded, _offsets_from_symbols
    if _offsets_loaded:
        return
    
    THREAD_OFFSETS, VCPU_OFFSETS, _offsets_from_symbols = _load_offsets_from_symbols()
    _offsets_loaded = True
    
    if _offsets_from_symbols:
        cprint('INFO', 'Loaded offsets from libphoton.so symbols')
    else:
        cprint('WARNING', 'Using default offsets (symbols not found)')

# =============================================================================
# Coroutine info access
# =============================================================================

class PhotonThread:
    """Coroutine info accessor (pure memory read, no type symbols needed)"""
    
    def __init__(self, addr):
        _ensure_offsets_loaded()
        self.addr = int(addr)
    
    def __int__(self):
        return self.addr
    
    def __str__(self):
        return hex(self.addr)
    
    def next(self):
        return read_ptr(self.addr + THREAD_OFFSETS['next'])
    
    def prev(self):
        return read_ptr(self.addr + THREAD_OFFSETS['prev'])
    
    def vcpu(self):
        return read_ptr(self.addr + THREAD_OFFSETS['vcpu'])
    
    def stack_ptr(self):
        return read_ptr(self.addr + THREAD_OFFSETS['stack_ptr'])
    
    def state(self):
        return read_u16(self.addr + THREAD_OFFSETS['state'])
    
    def state_name(self):
        return STATE_NAMES.get(self.state(), f'UNKNOWN({self.state()})')
    
    def get_saved_registers(self):
        """
        Restore registers from saved stack.
        Stack layout (set by Stack::init):
            [Stack._ptr + 0]  = saved_rbp (or th pointer)
            [Stack._ptr + 8]  = return_addr (rip)
        """
        sp = self.stack_ptr()
        if sp == 0:
            return None, None, None
        
        if is_aarch64():
            # AArch64 layout may differ, needs confirmation
            rbp = read_ptr(sp)
            rip = read_ptr(sp + 8)
            rsp = sp + 16
        else:
            # x86-64
            rbp = read_ptr(sp)
            rip = read_ptr(sp + 8)
            rsp = sp + 16
        
        return rsp, rbp, rip

def get_sleepq_threads(vcpu_addr):
    """Get all coroutines in sleep queue"""
    _ensure_offsets_loaded()
    if vcpu_addr == 0:
        return []
    
    # SleepQueue contains a std::vector<thread*>
    # std::vector memory layout: [_M_start, _M_finish, _M_end_of_storage]
    sleepq_offset = VCPU_OFFSETS.get('sleepq', 16)
    
    # Read vector's _M_start and _M_finish
    # vector._M_impl._M_start is at vector start
    # vector._M_impl._M_finish is at vector start + 8
    start = read_ptr(vcpu_addr + sleepq_offset)
    finish = read_ptr(vcpu_addr + sleepq_offset + 8)
    
    if start == 0 or finish == 0 or finish < start:
        return []
    
    count = (finish - start) // 8  # pointer size
    threads = []
    for i in range(min(count, 1000)):  # prevent infinite loop
        th = read_ptr(start + i * 8)
        if th != 0:
            threads.append(th)
    
    return threads

# =============================================================================
# VCPU and thread discovery (supports both live and coredump)
# =============================================================================

def is_coredump():
    """Check if debugging a coredump file"""
    try:
        # In coredump, function calls will fail
        gdb.parse_and_eval("(void)0")
        # Try a simple expression that only works in live process
        inferior = gdb.selected_inferior()
        # Check if process is running
        return not inferior.threads() or not any(t.is_running() for t in inferior.threads())
    except:
        return True

def get_pvcpu_head():
    """
    Get the head of pvcpu intrusive list.
    pvcpu is a static member: photon::vcpu_t::pvcpu
    intrusive_list has __prev_ptr and __next_ptr as the head node.
    """
    # Try different symbol names
    for symbol in [
        "'photon::vcpu_t::pvcpu'",
        "photon::vcpu_t::pvcpu",
        "'pvcpu'",
        "_ZN6photon6vcpu_t5pvcpuE",  # Mangled name
    ]:
        try:
            addr = int(gdb.parse_and_eval(f"&{symbol}"))
            if addr != 0:
                return addr
        except:
            continue
    return 0

def get_all_vcpus():
    """
    Traverse pvcpu intrusive_list to get all vcpus.
    
    intrusive_list<vcpu_t, false> has a single member: vcpu_t* node
    which points to the first element. When empty, node == nullptr.
    
    vcpu_t inherits from intrusive_list_node<vcpu_t>, which has
    __prev_ptr and __next_ptr at offsets list_node_prev and list_node_next.
    """
    _ensure_offsets_loaded()
    vcpus = []
    head = get_pvcpu_head()
    if head == 0:
        return vcpus
    
    # intrusive_list has only one member: NodeType* node (at offset 0)
    # This points to the first vcpu_t in the list
    first_vcpu = read_ptr(head)
    
    if first_vcpu == 0:
        return vcpus  # Empty list
    
    # Traverse the circular list
    # Each vcpu_t has intrusive_list_node members at list_node_prev and list_node_next offsets
    visited = set()
    current = first_vcpu
    
    while current != 0 and current not in visited:
        visited.add(current)
        vcpus.append(current)
        
        # Get next vcpu: current->__next_ptr
        # __next_ptr is at offset list_node_next within vcpu_t
        next_vcpu = read_ptr(current + VCPU_OFFSETS.get('list_node_next', 80))
        
        # Circular list: ends when next points back to first
        if next_vcpu == first_vcpu:
            break
        
        current = next_vcpu
        
        if len(vcpus) > 100:  # Safety limit
            break
    
    return vcpus

def get_runq_threads(vcpu_addr):
    """
    Get all threads in the run queue.
    Run queue is a circular list starting from idle_worker.
    """
    _ensure_offsets_loaded()
    if vcpu_addr == 0:
        return []
    
    idle_worker = read_ptr(vcpu_addr + VCPU_OFFSETS.get('idle_worker', 48))
    if idle_worker == 0:
        return []
    
    threads = []
    visited = set()
    current = idle_worker
    
    while current != 0 and current not in visited:
        visited.add(current)
        threads.append(current)
        current = read_ptr(current + THREAD_OFFSETS['next'])
        
        # Circular list ends when we return to idle_worker
        if current == idle_worker:
            break
        
        if len(threads) > 10000:  # Safety limit
            break
    
    return threads

def get_standbyq_threads(vcpu_addr):
    """
    Get all threads in standby queue.
    
    standbyq is a thread_list which inherits from intrusive_list<thread>.
    Layout: [intrusive_list<thread> (8 bytes: thread* node), spinlock lock]
    So standbyq.node is at standbyq_offset, pointing to first thread.
    """
    _ensure_offsets_loaded()
    if vcpu_addr == 0:
        return []
    
    standbyq_offset = VCPU_OFFSETS.get('standbyq', 56)
    
    # thread_list inherits from intrusive_list<thread>
    # intrusive_list<thread> has: thread* node (at offset 0)
    first_thread = read_ptr(vcpu_addr + standbyq_offset)
    
    if first_thread == 0:
        return []
    
    threads = []
    visited = set()
    current = first_thread
    
    while current != 0 and current not in visited:
        visited.add(current)
        threads.append(current)
        
        # thread inherits from intrusive_list_node<thread>
        # __next_ptr is at THREAD_OFFSETS['next']
        next_thread = read_ptr(current + THREAD_OFFSETS['next'])
        
        # Circular list: ends when next points back to first
        if next_thread == first_thread:
            break
        
        current = next_thread
        
        if len(threads) > 10000:
            break
    
    return threads

def try_call_gdb_helper(func_name, *args):
    """Try calling GDB helper function (only works for live process)"""
    try:
        if args:
            arg_str = ', '.join(f'(void*){a}' for a in args)
            return int(gdb.parse_and_eval(f"(unsigned long){func_name}({arg_str})"))
        else:
            return int(gdb.parse_and_eval(f"(unsigned long){func_name}()"))
    except:
        return None

def get_current_thread():
    """Get current coroutine pointer (for live process)"""
    # Method 1: Try calling helper function (live process)
    result = try_call_gdb_helper('gdb_get_current_thread')
    if result is not None and result != 0:
        return result
    
    # Method 2: Read TLS variable directly
    try:
        return int(gdb.parse_and_eval("(unsigned long)photon::CURRENT"))
    except:
        pass
    
    # Method 3: Try other variable names
    for var in ['CURRENT', '::CURRENT', 'photon::thread::CURRENT']:
        try:
            return int(gdb.parse_and_eval(f"(unsigned long){var}"))
        except:
            continue
    
    return 0

# =============================================================================
# Manual stack unwinding (without modifying registers)
# =============================================================================

def resolve_symbol(addr):
    """Resolve symbol for address"""
    if addr == 0:
        return "0x0"
    
    try:
        # Use GDB's symbol resolution
        result = gdb.execute(f"info symbol {addr}", to_string=True).strip()
        if "No symbol" not in result:
            return result.split('\n')[0]
    except:
        pass
    
    # Try using Python API
    try:
        block = gdb.block_for_pc(addr)
        if block and block.function:
            return block.function.name
    except:
        pass
    
    return f"0x{addr:x}"

def resolve_location(addr):
    """Resolve source location"""
    if addr == 0:
        return ""
    
    try:
        sal = gdb.find_pc_line(addr)
        if sal.symtab:
            return f"at {sal.symtab.filename}:{sal.line}"
    except:
        pass
    
    return ""

def manual_backtrace(rsp, rbp, rip, max_depth=30):
    """
    Manual stack unwinding without modifying GDB registers.
    Returns list of (frame_index, ip_addr) tuples for further formatting.
    
    x86-64 stack frame layout (with frame pointer):
        [rbp + 8]  = return address
        [rbp + 0]  = saved rbp (points to previous frame)
    """
    if rip == 0:
        return []
    
    frames = []
    cur_ip = rip
    cur_bp = rbp
    
    for i in range(max_depth):
        frames.append((i, cur_ip))
        
        # Check if reached stack bottom
        if cur_bp == 0:
            break
        
        # Read previous frame
        next_bp = read_ptr(cur_bp)
        next_ip = read_ptr(cur_bp + 8)
        
        # Validity check
        if next_ip == 0:
            break
        if next_bp != 0 and next_bp <= cur_bp:
            # Stack should grow towards higher addresses (during unwinding)
            break
        
        cur_bp = next_bp
        cur_ip = next_ip
    
    return frames

def manual_backtrace_gdb_style(rsp, rbp, rip, max_depth=30):
    """
    Manual stack unwinding with GDB-style formatted output.
    """
    frames = manual_backtrace(rsp, rbp, rip, max_depth)
    
    if not frames:
        return ["(no stack frames)"]
    
    return [format_backtrace_frame(idx, addr) for idx, addr in frames]

def get_gdb_threads_registers():
    """
    Get registers and vcpu info from all GDB threads (OS threads).
    Returns a list of (gdb_thread_id, rsp, rbp, rip, vcpu_addr) tuples.
    vcpu_addr is obtained by reading CURRENT thread's vcpu pointer on that GDB thread.
    """
    result = []
    try:
        inferior = gdb.selected_inferior()
        current_thread = gdb.selected_thread()
        
        for thread in inferior.threads():
            try:
                thread.switch()
                if is_aarch64():
                    rsp = int(gdb.parse_and_eval("$sp"))
                    rbp = int(gdb.parse_and_eval("$x29"))
                    rip = int(gdb.parse_and_eval("$pc"))
                else:
                    rsp = int(gdb.parse_and_eval("$rsp"))
                    rbp = int(gdb.parse_and_eval("$rbp"))
                    rip = int(gdb.parse_and_eval("$rip"))
                
                # Try to get vcpu address from CURRENT thread on this GDB thread
                vcpu_addr = 0
                try:
                    current = get_current_thread()
                    if current != 0:
                        th = PhotonThread(current)
                        vcpu_addr = th.vcpu()
                except:
                    pass
                
                result.append((thread.num, rsp, rbp, rip, vcpu_addr))
            except:
                continue
        
        # Restore original thread
        if current_thread:
            current_thread.switch()
    except:
        pass
    
    return result

# =============================================================================
# Collect all coroutines
# =============================================================================

def collect_all_threads():
    """
    Collect all coroutines from all vCPUs.
    Supports both live process and coredump.
    """
    threads = []
    visited = set()
    
    # Method 1: Try via pvcpu static variable (works for both live and coredump)
    # This collects threads from ALL vCPUs in one call
    vcpus = get_all_vcpus()
    if vcpus:
        return collect_threads_from_vcpus(vcpus)
    
    # Method 2: Fallback to CURRENT (single vCPU, live process only)
    current = get_current_thread()
    if current != 0:
        return collect_threads_from_current(current)
    
    # Neither method worked
    head = get_pvcpu_head()
    if head == 0:
        cprint('ERROR', "Cannot find pvcpu symbol. Check if debug symbols are loaded.")
    else:
        cprint('WARNING', "pvcpu list is empty. Photon may not be initialized yet.")
    return threads

def collect_threads_from_vcpus(vcpus):
    """
    Collect threads from all vCPUs via pvcpu list.
    Works for both live process (multi-vCPU) and coredump.
    """
    threads = []
    visited = set()
    
    # Get registers from all GDB threads for RUNNING/CURRENT coroutines
    gdb_thread_regs = get_gdb_threads_registers()
    running_coroutines = []  # Will collect RUNNING coroutines first, then assign regs
    
    for vcpu_idx, vcpu_addr in enumerate(vcpus):
        # Get threads from runq (via idle_worker)
        runq_threads = get_runq_threads(vcpu_addr)
        for th_addr in runq_threads:
            if th_addr in visited:
                continue
            visited.add(th_addr)
            th = PhotonThread(th_addr)
            state_code = th.state()
            state_name = STATE_NAMES.get(state_code, f'UNK({state_code})')
            
            if state_code == 1:  # RUNNING
                # Collect RUNNING coroutine, will assign registers later
                running_coroutines.append({
                    'addr': th_addr,
                    'state': 'RUNNING',
                    'vcpu_idx': vcpu_idx,
                    'vcpu_addr': vcpu_addr,
                })
            else:
                rsp, rbp, rip = th.get_saved_registers()
                threads.append({
                    'addr': th_addr,
                    'state': state_name,
                    'vcpu_idx': vcpu_idx,
                    'vcpu_addr': vcpu_addr,
                    'rsp': rsp or 0,
                    'rbp': rbp or 0,
                    'rip': rip or 0,
                })
        
        # Get threads from sleepq
        for th_addr in get_sleepq_threads(vcpu_addr):
            if th_addr in visited:
                continue
            visited.add(th_addr)
            th = PhotonThread(th_addr)
            rsp, rbp, rip = th.get_saved_registers()
            threads.append({
                'addr': th_addr,
                'state': 'SLEEP',
                'vcpu_idx': vcpu_idx,
                'vcpu_addr': vcpu_addr,
                'rsp': rsp or 0,
                'rbp': rbp or 0,
                'rip': rip or 0,
            })
        
        # Get threads from standbyq
        for th_addr in get_standbyq_threads(vcpu_addr):
            if th_addr in visited:
                continue
            visited.add(th_addr)
            th = PhotonThread(th_addr)
            rsp, rbp, rip = th.get_saved_registers()
            threads.append({
                'addr': th_addr,
                'state': 'STANDBY',
                'vcpu_idx': vcpu_idx,
                'vcpu_addr': vcpu_addr,
                'rsp': rsp or 0,
                'rbp': rbp or 0,
                'rip': rip or 0,
            })
    
    # Assign GDB thread registers to RUNNING coroutines
    # Match by vcpu address for accurate vCPU <-> GDB thread mapping
    used_gdb_threads = set()
    
    # Build vcpu_addr -> gdb_thread mapping
    vcpu_to_gdb = {}  # vcpu_addr -> (gdb_tid, rsp, rbp, rip)
    for gdb_tid, rsp, rbp, rip, vcpu_addr in gdb_thread_regs:
        if vcpu_addr != 0:
            vcpu_to_gdb[vcpu_addr] = (gdb_tid, rsp, rbp, rip)
    
    for rc in running_coroutines:
        vcpu_idx = rc['vcpu_idx']
        vcpu_addr = rc['vcpu_addr']
        assigned = False
        
        # Try to find matching GDB thread by vcpu address
        if vcpu_addr in vcpu_to_gdb:
            gdb_tid, rsp, rbp, rip = vcpu_to_gdb[vcpu_addr]
            if gdb_tid not in used_gdb_threads:
                used_gdb_threads.add(gdb_tid)
                threads.insert(0, {
                    'addr': rc['addr'],
                    'state': 'CURRENT',
                    'vcpu_idx': vcpu_idx,
                    'vcpu_addr': vcpu_addr,
                    'gdb_thread': gdb_tid,
                    'rsp': rsp,
                    'rbp': rbp,
                    'rip': rip,
                })
                assigned = True
        
        # Fallback: try any available GDB thread
        if not assigned:
            for gdb_tid, rsp, rbp, rip, _ in gdb_thread_regs:
                if gdb_tid not in used_gdb_threads:
                    used_gdb_threads.add(gdb_tid)
                    threads.insert(0, {
                        'addr': rc['addr'],
                        'state': 'CURRENT',
                        'vcpu_idx': vcpu_idx,
                        'vcpu_addr': vcpu_addr,
                        'gdb_thread': gdb_tid,
                        'rsp': rsp,
                        'rbp': rbp,
                        'rip': rip,
                    })
                    assigned = True
                    break
        
        if not assigned:
            # No GDB thread available (coredump case), mark as RUNNING
            threads.insert(0, {
                'addr': rc['addr'],
                'state': 'RUNNING',
                'vcpu_idx': vcpu_idx,
                'vcpu_addr': vcpu_addr,
                'rsp': 0,
                'rbp': 0,
                'rip': 0,
                'note': 'No GDB thread context available',
            })
    
    # Update gdb_thread for all non-CURRENT threads based on vcpu_addr
    for t in threads:
        if 'gdb_thread' not in t and 'vcpu_addr' in t:
            if t['vcpu_addr'] in vcpu_to_gdb:
                t['gdb_thread'] = vcpu_to_gdb[t['vcpu_addr']][0]
    
    return threads

def collect_threads_from_current(current):
    """
    Collect threads starting from CURRENT (for live process).
    This is the original method that works well for live debugging.
    """
    threads = []
    current_th = PhotonThread(current)
    
    # Get vCPU address and GDB thread number
    vcpu_addr = current_th.vcpu()
    gdb_thread = 1  # Default to thread 1 for single vCPU
    try:
        gdb_thread = gdb.selected_thread().num
    except:
        pass
    
    # Current coroutine (use current CPU registers)
    try:
        if is_aarch64():
            cur_rsp = int(gdb.parse_and_eval("$sp"))
            cur_rbp = int(gdb.parse_and_eval("$x29"))
            cur_rip = int(gdb.parse_and_eval("$pc"))
        else:
            cur_rsp = int(gdb.parse_and_eval("$rsp"))
            cur_rbp = int(gdb.parse_and_eval("$rbp"))
            cur_rip = int(gdb.parse_and_eval("$rip"))
    except:
        cur_rsp, cur_rbp, cur_rip = 0, 0, 0
    
    threads.append({
        'addr': current,
        'state': 'CURRENT',
        'vcpu_addr': vcpu_addr,
        'gdb_thread': gdb_thread,
        'rsp': cur_rsp,
        'rbp': cur_rbp,
        'rip': cur_rip,
    })
    
    # Other coroutines in ready queue
    visited = {current}
    p = current_th.next()
    while p != 0 and p != current and p not in visited:
        visited.add(p)
        th = PhotonThread(p)
        rsp, rbp, rip = th.get_saved_registers()
        threads.append({
            'addr': p,
            'state': 'READY',
            'vcpu_addr': vcpu_addr,
            'gdb_thread': gdb_thread,
            'rsp': rsp or 0,
            'rbp': rbp or 0,
            'rip': rip or 0,
        })
        p = th.next()
    
    # Coroutines in sleep queue
    if vcpu_addr != 0:
        for th_addr in get_sleepq_threads(vcpu_addr):
            if th_addr not in visited:
                visited.add(th_addr)
                th = PhotonThread(th_addr)
                rsp, rbp, rip = th.get_saved_registers()
                threads.append({
                    'addr': th_addr,
                    'state': 'SLEEP',
                    'vcpu_addr': vcpu_addr,
                    'gdb_thread': gdb_thread,
                    'rsp': rsp or 0,
                    'rbp': rbp or 0,
                    'rip': rip or 0,
                })
    
    return threads

# =============================================================================
# GDB-style output formatting
# =============================================================================

def format_frame_brief(addr):
    """
    Format a single frame in GDB style for thread listing.
    Example: main () at main.cpp:10
    """
    if addr == 0:
        return "(no frame)"
    
    sym = resolve_symbol(addr)
    loc = resolve_location(addr)
    
    # Extract function name for brief display
    if ' in section ' in sym:
        sym = sym.split(' in section ')[0]
    if ' + ' in sym:
        sym = sym.split(' + ')[0] + " ()"
    elif '(' not in sym and sym != f"0x{addr:x}":
        sym = sym + " ()"
    
    if loc:
        return f"{sym} {loc}"
    return sym

def format_backtrace_frame(idx, addr):
    """
    Format a backtrace frame in GDB style.
    Example: #0  0x00007ffff7a5e017 in __GI___poll (fds=0x555555559010) at poll.c:29
    """
    if addr == 0:
        return f"#{idx}  0x{0:016x} in ?? ()"
    
    sym = resolve_symbol(addr)
    loc = resolve_location(addr)
    
    # Clean up symbol format
    if ' in section ' in sym:
        sym = sym.split(' in section ')[0]
    if ' + ' in sym:
        base = sym.split(' + ')[0]
        offset = sym.split(' + ')[1]
        sym = f"{base}+{offset}"
    elif '(' not in sym and not sym.startswith('0x'):
        sym = sym + " ()"
    
    if loc:
        return f"#{idx}  0x{addr:016x} in {sym} {loc}"
    return f"#{idx}  0x{addr:016x} in {sym}"

# =============================================================================
# GDB commands
# =============================================================================

class PhotonLs(gdb.Command):
    """
    List all Photon threads (similar to GDB 'info threads').
    Usage: photon_ls
    """
    
    def __init__(self):
        gdb.Command.__init__(self, "photon_ls", gdb.COMMAND_STACK, gdb.COMPLETE_NONE)
    
    def invoke(self, arg, tty):
        global _selected_thread_idx
        threads = collect_all_threads()
        
        if not threads:
            cprint('WARNING', "No photon threads found")
            return
        
        # Reset selection if out of range
        if _selected_thread_idx >= len(threads):
            _selected_thread_idx = 0
        
        # Mimic GDB's "info threads" format
        print()
        
        for i, t in enumerate(threads):
            state = t['state']
            addr = t['addr']
            rip = t['rip']
            color = CMAP.get(state, '')
            
            # Current selected thread marker (like GDB's *)
            marker = '*' if i == _selected_thread_idx else ' '
            
            # vCPU and GDB thread info
            extra_info = ""
            if 'gdb_thread' in t and 'vcpu_addr' in t:
                extra_info += f" vCPU {t['gdb_thread']} ({t['vcpu_addr']:#x})"
            
            # Frame info
            frame_str = format_frame_brief(rip)
            if state == 'RUNNING' and rip == 0:
                frame_str = "(no context available)"
            
            # Format like: * 1    Thread 0x555... RUNNING vCPU 0 (LWP in thread 1)  main ()
            print(f"{marker} {i:<4} {color}Thread {addr:#x}{bcolors.ENDC} "
                  f"{color}{state:<8}{bcolors.ENDC}{extra_info}  {frame_str}")
            
            # Show note if present
            if 'note' in t:
                print(f"        {t['note']}")
        
        print()


class PhotonBt(gdb.Command):
    """
    Show backtrace for specified Photon thread (similar to GDB 'bt').
    Usage: photon_bt [index]
    If index is omitted, shows backtrace for current selected thread (set by photon_fr).
    """
    
    def __init__(self):
        gdb.Command.__init__(self, "photon_bt", gdb.COMMAND_STACK, gdb.COMPLETE_NONE)
    
    def invoke(self, arg, tty):
        global _selected_thread_idx
        threads = collect_all_threads()
        
        if not threads:
            cprint('WARNING', "No photon threads found")
            return
        
        # Use argument if provided, otherwise use selected thread
        try:
            if arg.strip():
                idx = int(arg.strip())
            else:
                idx = _selected_thread_idx
        except ValueError:
            print("Usage: photon_bt [index]")
            return
        
        if idx < 0 or idx >= len(threads):
            print(f"Invalid index. Valid range: 0-{len(threads)-1}")
            return
        
        t = threads[idx]
        state = t['state']
        color = CMAP.get(state, '')
        
        # Build extra info string
        extra_info = ""
        if 'gdb_thread' in t and 'vcpu_addr' in t:
            extra_info += f" vCPU {t['gdb_thread']} ({t['vcpu_addr']:#x})"
        
        # Header like GDB's thread switch message
        print(f"\nThread {idx}, {t['addr']:#x} ({color}{state}{bcolors.ENDC}){extra_info}")
        
        if t['rip'] == 0:
            if 'note' in t:
                print(f"({t['note']})")
            else:
                print("(No stack context available)")
        else:
            frames = manual_backtrace_gdb_style(t['rsp'], t['rbp'], t['rip'])
            for frame in frames:
                print(frame)
        
        print()


class PhotonPs(gdb.Command):
    """
    Show backtrace for all Photon threads (similar to GDB 'thread apply all bt').
    Usage: photon_ps
    """
    
    def __init__(self):
        gdb.Command.__init__(self, "photon_ps", gdb.COMMAND_STACK, gdb.COMPLETE_NONE)
    
    def invoke(self, arg, tty):
        threads = collect_all_threads()
        
        if not threads:
            cprint('WARNING', "No photon threads found")
            return
        
        for i, t in enumerate(threads):
            state = t['state']
            color = CMAP.get(state, '')
            
            # Build extra info string
            extra_info = ""
            if 'gdb_thread' in t and 'vcpu_addr' in t:
                extra_info += f" vCPU {t['gdb_thread']} ({t['vcpu_addr']:#x})"
            
            # Header like GDB's thread switch message
            print(f"\nThread {i}, {t['addr']:#x} ({color}{state}{bcolors.ENDC}){extra_info}")
            
            if t['rip'] == 0:
                if 'note' in t:
                    print(f"({t['note']})")
                else:
                    print("(No stack context available)")
            else:
                frames = manual_backtrace_gdb_style(t['rsp'], t['rbp'], t['rip'])
                for frame in frames:
                    print(frame)
        
        print()


class PhotonFr(gdb.Command):
    """
    Select a Photon thread (similar to GDB 'thread' command).
    Usage: photon_fr <index>
    After selection, photon_bt without arguments will show the selected thread's backtrace.
    """
    
    def __init__(self):
        gdb.Command.__init__(self, "photon_fr", gdb.COMMAND_STACK, gdb.COMPLETE_NONE)
    
    def invoke(self, arg, tty):
        global _selected_thread_idx
        threads = collect_all_threads()
        
        if not threads:
            cprint('WARNING', "No photon threads found")
            return
        
        if not arg.strip():
            # Show current selection
            print(f"Current selected thread: {_selected_thread_idx}")
            return
        
        try:
            idx = int(arg.strip())
        except ValueError:
            print("Usage: photon_fr <index>")
            return
        
        if idx < 0 or idx >= len(threads):
            print(f"Invalid index. Valid range: 0-{len(threads)-1}")
            return
        
        _selected_thread_idx = idx
        t = threads[idx]
        state = t['state']
        color = CMAP.get(state, '')
        
        # Build extra info string
        extra_info = ""
        if 'gdb_thread' in t and 'vcpu_addr' in t:
            extra_info += f" vCPU {t['gdb_thread']} ({t['vcpu_addr']:#x})"
        
        # Show switched message like GDB
        print(f"[Switching to Thread {idx}, {t['addr']:#x} ({color}{state}{bcolors.ENDC}){extra_info}]")
        
        # Show top frame
        if t['rip'] != 0:
            print(format_backtrace_frame(0, t['rip']))


# =============================================================================
# Legacy commands (for compatibility)
# =============================================================================

class PhotonInit(gdb.Command):
    """[Legacy] No longer needed, kept for backward compatibility"""
    def __init__(self):
        gdb.Command.__init__(self, "photon_init", gdb.COMMAND_STACK, gdb.COMPLETE_NONE)
    def invoke(self, arg, tty):
        cprint('INFO', "photon_init is no longer needed in v2. Just use photon_ls/photon_ps directly.")


class PhotonFini(gdb.Command):
    """[Legacy] No longer needed, kept for backward compatibility"""
    def __init__(self):
        gdb.Command.__init__(self, "photon_fini", gdb.COMMAND_STACK, gdb.COMPLETE_NONE)
    def invoke(self, arg, tty):
        cprint('INFO', "photon_fini is no longer needed in v2.")


class PhotonRst(gdb.Command):
    """[Legacy] No longer needed, kept for backward compatibility"""
    def __init__(self):
        gdb.Command.__init__(self, "photon_rst", gdb.COMMAND_STACK, gdb.COMPLETE_NONE)
    def invoke(self, arg, tty):
        cprint('INFO', "photon_rst is no longer needed in v2. Registers are not modified.")


class PhotonCurrent(gdb.Command):
    """
    Show current selected Photon thread info (selected by photon_fr).
    Usage: photon_current
    """
    def __init__(self):
        gdb.Command.__init__(self, "photon_current", gdb.COMMAND_STACK, gdb.COMPLETE_NONE)
    
    def invoke(self, arg, tty):
        global _selected_thread_idx
        threads = collect_all_threads()
        
        if not threads:
            cprint('WARNING', "No photon threads found")
            return
        
        if _selected_thread_idx >= len(threads):
            _selected_thread_idx = 0
        
        t = threads[_selected_thread_idx]
        state = t['state']
        
        # Build extra info
        extra_info = ""
        if 'gdb_thread' in t and 'vcpu_addr' in t:
            extra_info += f" vCPU {t['gdb_thread']} ({t['vcpu_addr']:#x})"
        
        # Output thread info only
        cprint(state, f"[{_selected_thread_idx}] {t['addr']:#x}{extra_info}")


# =============================================================================
# Initialization
# =============================================================================

PhotonLs()
PhotonBt()
PhotonPs()
PhotonFr()
PhotonCurrent()
PhotonInit()
PhotonFini()
PhotonRst()

cprint('INFO', 'Photon-GDB-extension v2 loaded')
cprint('INFO', 'Commands: photon_ls, photon_fr <n>, photon_bt [n], photon_ps, photon_current')
cprint('INFO', 'Use photon_fr to select thread, then photon_bt to show its backtrace')
