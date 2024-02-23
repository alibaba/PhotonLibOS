#!/usr/bin/python
# -*- coding: utf-8 -*-

import gdb


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
    'WARNING': bcolors.HEADER,
    'SWITCH': bcolors.BOLD,
    'INFO': bcolors.OKGREEN,
}

enabling = False
photon = []

def get_arch():
    frame = gdb.selected_frame()
    arch = frame.architecture()
    return arch.name()

def get_regs(arch):
    regs = {}
    if arch == 'aarch64':
        regs['sp'] = '$sp'
        regs['bp'] = '$x29'
        regs['ip'] = '$pc'
    else:
        regs['sp'] = '$rsp'
        regs['bp'] = '$rbp'
        regs['ip'] = '$rip'

    return regs

def cprint(stat, *args):
    print('{}{}{} {}'.format(CMAP[stat], stat, bcolors.ENDC,
                             ' '.join(str(x) for x in args)))


def get_next_ready(p):
    return gdb.parse_and_eval("(photon::thread*)%s" % p.dereference()['__next_ptr'])


def get_current():
    return gdb.parse_and_eval("(photon::thread*)photon::CURRENT")


def get_vcpu(p):
    return p.dereference()['vcpu'].dereference()


def get_sleepq(vcpu):
    return vcpu['sleepq']['q']


def in_sleep(q):
    size = q['_M_impl']['_M_finish'] - q['_M_impl']['_M_start']
    return [(q['_M_impl']['_M_start'][i]) for i in range(size)]


def switch_to_ph(regs, rsp, rbp, rip):
    cprint('SWITCH', "to {} {} {}".format(hex(rsp), hex(rbp), hex(rip)))
    gdb.parse_and_eval("{}={}".format(regs['sp'], rsp))
    gdb.parse_and_eval("{}={}".format(regs['bp'], rbp))
    gdb.parse_and_eval("{}={}".format(regs['ip'], rip))


def get_u64_ptr(p):
    return int(gdb.parse_and_eval("(uint64_t)({})".format(p)))


def get_u64_val(p):
    return int(gdb.parse_and_eval("*(uint64_t*)({})".format(p)))


def get_u64_reg(p):
    return get_u64_ptr(p)


def set_u64_reg(l, r):
    return gdb.parse_and_eval("{} = (uint64_t)({})".format(l, r))


def get_stkregs(p):
    t = get_u64_ptr(p['stack']['_ptr'])
    rsp = t + 8
    rip = get_u64_val(t + 8)
    rbp = get_u64_val(t)
    return rsp, rbp, rip


def load_photon_threads():
    global photon
    if enabling:
        return
    photon = []
    c = get_current()
    if c == gdb.parse_and_eval("0"):
        return
    photon.append(('CURRENT', c, get_u64_reg('$saved_rsp'),
                   get_u64_reg('$saved_rbp'), get_u64_reg('$saved_rip')))
    p = get_next_ready(c)
    while p != c:
        rsp, rbp, rip = get_stkregs(p)
        photon.append(('READY', p, rsp, rbp, rip))
        p = get_next_ready(p)
    vcpu = get_vcpu(c)
    for t in in_sleep(get_sleepq(vcpu)):
        rsp, rbp, rip = get_stkregs(t)
        photon.append(('SLEEP', t, rsp, rbp, rip))
    return


class PhotonThreads(gdb.Command):
    def __init__(self):
        gdb.Command.__init__(self, "photon_current",
                             gdb.COMMAND_STACK, gdb.COMPLETE_NONE)

    def invoke(self, arg, tty):
        photon_init()
        cprint("CURRENT", get_current().dereference())


class PhotonLs(gdb.Command):
    def __init__(self):
        gdb.Command.__init__(
            self, "photon_ls", gdb.COMMAND_STACK, gdb.COMPLETE_NONE)

    def invoke(self, arg, tty):
        photon_init()
        for i, (stat, pth, rsp, rbp, rbi) in enumerate(photon):
            cprint(
                stat, '[{}]'.format(i), pth, hex(rsp), hex(rbp), hex(rbi))


class PhotonFr(gdb.Command):
    def __init__(self):
        gdb.Command.__init__(
            self, "photon_fr", gdb.COMMAND_STACK, gdb.COMPLETE_NONE)

    def invoke(self, arg, tty):
        if not enabling:
            print("Photon debugger not init")
            return
        i = int(arg)
        if i < 0 or i > len(photon):
            print("No such photon thread")
            return

        arch = get_arch()
        regs = get_regs(arch)
        switch_to_ph(regs, photon[i][2], photon[i][3], photon[i][4])


def photon_init():
    global photon

    arch = get_arch()
    regs = get_regs(arch)
    set_u64_reg('$saved_rsp', regs['sp'])
    set_u64_reg('$saved_rbp', regs['bp'])
    set_u64_reg('$saved_rip', regs['ip'])
    load_photon_threads()
    if len(photon) == 0:
        return


class PhotonInit(gdb.Command):
    def __init__(self):
        gdb.Command.__init__(self, "photon_init",
                             gdb.COMMAND_STACK, gdb.COMPLETE_NONE)

    def invoke(self, arg, tty):
        global enabling
        photon_init()
        enabling = True
        cprint('WARNING', "Entered photon thread lookup mode. PLEASE do not trying step-in or continue before `photon_fini`")


def photon_restore():
    if not enabling:
        return
    arch = get_arch()
    regs = get_regs(arch)
    set_u64_reg(regs['sp'], '$saved_rsp')
    set_u64_reg(regs['bp'], '$saved_rbp')
    set_u64_reg(regs['ip'], '$saved_rip')


class PhotonRestore(gdb.Command):
    def __init__(self):
        gdb.Command.__init__(self, "photon_rst",
                             gdb.COMMAND_STACK, gdb.COMPLETE_NONE)

    def invoke(self, arg, tty):
        photon_restore()


class PhotonFini(gdb.Command):
    def __init__(self):
        gdb.Command.__init__(self, "photon_fini",
                             gdb.COMMAND_STACK, gdb.COMPLETE_NONE)

    def invoke(self, arg, tty):
        global photon
        global enabling
        if not enabling:
            return
        photon_restore()
        photon = []
        enabling = False
        cprint('WARNING', "Finished photon thread lookup mode.")


PhotonInit()
PhotonFini()
PhotonRestore()
PhotonThreads()
PhotonLs()
PhotonFr()

cprint('INFO', 'Photon-GDB-extension loaded')
