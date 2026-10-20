"""Check actual PE exports against the reconstruction manifest, without loading it."""
import argparse
import hashlib
import json
import struct
from pathlib import Path

def _resource_capabilities(data, identifier, expected_magic, required_abi, required_features, label):
    """Read a versioned capability resource; never execute the DLL."""
    def read(fmt, at):
        n=struct.calcsize('<'+fmt)
        if at<0 or at+n>len(data):raise ValueError('Truncated archive capability PE')
        return struct.unpack_from('<'+fmt,data,at)
    if data[:2]!=b'MZ':raise ValueError('Missing MZ header')
    pe,=read('I',0x3c)
    if data[pe:pe+4]!=b'PE\0\0' or read('H',pe+4)[0]!=0x14c:raise ValueError('Expected x86 PE')
    opt=pe+24
    if read('H',opt)[0]!=0x10b:raise ValueError('Expected PE32')
    sections=[read('IIII',opt+read('H',pe+20)[0]+40*i+8) for i in range(read('H',pe+6)[0])]
    def mapped(rva,n):
        for _,va,size,raw in sections:
            if va<=rva and rva-va+n<=size and raw+rva-va+n<=len(data):return raw+rva-va
        raise ValueError('Unmapped archive capability resource')
    base,size=read('II',opt+112)
    def relative(at,n):
        if at<0 or at+n>size:raise ValueError('Invalid resource offset')
        return mapped(base+at,n)
    def child(at,identifier,directory):
        named,numbered=read('HH',relative(at,16)+12)
        for i in range(named+numbered):
            name,target=read('II',relative(at+16+8*i,8))
            if name==identifier:
                if bool(target&0x80000000)!=directory:raise ValueError('Invalid resource tree')
                return target&0x7fffffff
        raise ValueError(f'TTPCOMM release lacks {label} capability metadata; publish the updated DLL first')
    at=child(child(child(0,10,True),identifier,True),0,False)
    payload,n=read('II',relative(at,16))
    if n!=16:raise ValueError('Invalid archive capability size')
    magic,version,abi,features=read('IIII',mapped(payload,n))
    if magic!=expected_magic or version!=1 or abi<required_abi or features&required_features!=required_features:
        raise ValueError(f'TTPCOMM lacks required {label} capabilities; publish the updated DLL first')
    return {'abi':abi,'features':features}

def archive_capabilities(data):
    return _resource_capabilities(data,503,0x41435054,3,16383,
        'archive/password/signature/7z BZip2/Zstandard')

def audio_capabilities(data):
    return _resource_capabilities(data,504,0x41415054,2,3,
        'audio ABI 2 encoder/effects')

def inspect_exports(path):
    data=path.read_bytes()
    def unpack(fmt,offset):return struct.unpack_from('<'+fmt,data,offset)
    if data[:2]!=b'MZ':raise ValueError('Missing MZ header')
    pe=unpack('I',0x3c)[0]
    if data[pe:pe+4]!=b'PE\0\0':raise ValueError('Missing PE signature')
    if unpack('H',pe+4)[0]!=0x14c:raise ValueError('Expected x86 machine')
    opt=pe+24
    if unpack('H',opt)[0]!=0x10b:raise ValueError('Expected PE32 optional header')
    sections=[unpack('IIII',opt+unpack('H',pe+20)[0]+40*i+8) for i in range(unpack('H',pe+6)[0])]
    def offset(rva):
        for virtual_size,virtual,raw_size,raw in sections:
            if virtual<=rva<virtual+raw_size:return raw+rva-virtual
        raise ValueError(f'Unmapped export RVA {rva:x}')
    def string(rva):
        p=offset(rva);return data[p:data.index(0,p)].decode('ascii')
    export,size=unpack('II',opt+96)
    base,count,names,addresses,nt,ot=unpack('IIIIII',offset(export)+16)
    named={unpack('H',offset(ot)+2*i)[0]:string(unpack('I',offset(nt)+4*i)[0]) for i in range(names)}
    out={}
    for i in range(count):
        address=unpack('I',offset(addresses)+i*4)[0]
        if not address:continue
        if export<=address<export+size:raise ValueError('Forwarder is not an independent implementation')
        out[base+i]={'name':named.get(i,''),'rva':address}
    return out
