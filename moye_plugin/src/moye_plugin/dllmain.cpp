#include <algorithm>
#include <iostream>
#include <vector>
#include <stack>
#include <windows.h>
#include <shlwapi.h>
#include <psapi.h>
#include "ollydbg-sdk/Plugin.h"
#include "Emulator.h"
#include "Analyzer.h"

#pragma comment(lib,"Shlwapi.lib")

using namespace std;

HWND g_hOllyDbg;			//OD��������
bool is_tracing;            //�Ƿ����ڸ���api����
bool emu_control;           //����ģ�����Ƿ���ͣ��������ѡ�ģ����api/�ڴ���ʷ�����
Analyzer a;                 //�����������ڷ����ָ��
DWORD hash_data_addr;       //��¼SP��hash����д��ĵ�ַ
DWORD hash_data_size;       //hash���ݵĳ��ȣ�1��2��4�ֽڣ�

DWORD cpuid_eax=0;            //ģ������hook��Щֵ
DWORD cpuid_ecx=0;
DWORD cpuid_edx=0;
DWORD cpuid_ebx=0;
DWORD rdtsc_eax=0;
DWORD rdtsc_edx=0;
bool special_ins_solver_control=0; //�����Ƿ�hookģ����cpuid��rdtscָ���ֵ(�Ƿ�ע�ᴦ��ص�)

/**
 * \brief �����޸�IAT��
 */
struct api_info
{
    //�����������������
    bool operator<(const api_info& v)
    {
        if (dll_name == v.dll_name) return api_name < v.api_name;
        else return dll_name < v.dll_name;
    }
    std::string dll_name;   //api�����ĸ�dll��dll����·����
    std::string api_name;   //api����������
    DWORD fix_addr;         //�ڴ˵�ַ�޸�iat����
    DWORD api_addr;         //���õ�api�ĵ�ַ
    DWORD type;             //1-call 2-jmp 3-mov eax 4-mov ecx 5-mov edx 6-mov ebx 7-mov ebp 8-mov esi 9-mov edi
};

/**
 * \brief ����GetLastError������Ϣ
 */
string GetLastErrorStr()
{
    DWORD err_code = GetLastError();
    if (err_code == 0)
    {
        puts("û�д�����Ϣ\n");
        return "";
    }
    char* buffer = nullptr;
    FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL, err_code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR)&buffer, 0, NULL);
    string str = buffer;
    str.resize(str.size() - 2); //ȥ��ĩβ�Ļ��з�\r\n
    LocalFree(buffer);
    return str;
}

/**
 * \brief ��ָ����ַ���ӱ�ǩ(ctrl cv��nck)
 * \param dump ת������
 */
DWORD __stdcall RenameCall(LPVOID lpThreadParameter)
{
    t_dump* dump = (t_dump*)lpThreadParameter;
    ulong select_addr = dump->sel0;
    if (select_addr == 0) return -1;
    uchar buf[MAXCMDSIZE];
    _Readmemory(buf, select_addr, MAXCMDSIZE, MM_SILENT);
    if (buf[0] != 0xE8) return -1;
    t_disasm td;
    if (strncmp(td.result, "call", 4)) return -1;
    char old_label[TEXTLEN] = { 0 };
    char new_label[TEXTLEN] = { 0 };
    _Findlabel(td.jmpaddr, old_label);
    if (_Gettext("�������ǩ��", new_label, 0, NM_NONAME, 0) != -1) {
        _Addtolist(td.jmpaddr, 0, "[moye]��ַ��%#08x    ��ǩ��%s    ԭʼ��%s", td.jmpaddr, new_label, old_label);
        _Insertname(td.jmpaddr, NM_LABEL, new_label);
    }
    return 0;
}

/**
 * \brief �ڵ�ǰ�����Խ��̿ռ��з���һƬ�ڴ�
 * \param lpThreadParameter δʹ�ô˲���
 * \return
 */
DWORD __stdcall AllocMemory(LPVOID lpThreadParameter)
{
    DWORD size = 0;
    if (_Getlong("�ڴ��С(ʮ������)", &size, 4, '0', DIA_HEXONLY) != 0)
    {
        return -1;
    }
    HANDLE hprocess = (HANDLE)_Plugingetvalue(VAL_HPROCESS);
    DWORD addr = (DWORD)VirtualAllocEx(hprocess, 0, size, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!addr)
    {
        _Addtolist(0, 1, "�����ڴ�ʧ�ܣ�%s", GetLastErrorStr().c_str());
        return -1;
    }
    _Listmemory();  //��ODˢ���ڴ��б���Ȼ����Ҳ����ղŷ�����ڴ�
    t_memory* memory = _Findmemory(addr);
    if (!memory)
    {
        _Addtolist(0, 1, "���ҷ�����ڴ�ʧ�ܣ�%s", GetLastErrorStr().c_str());
        VirtualFreeEx(hprocess, (LPVOID)addr, 0, MEM_RELEASE);
        return -1;
    }
    _Addtolist(addr, 0, "�����ڴ�ɹ� ��ַ��%08X ��С��%08x", addr, memory->size);
    _Setcpu(0, 0, addr, 0, CPU_DUMPFIRST);
    return 0;
}


/**
 * \brief �ϲ��ڴ��ʽdump�������ļ��������Գ���Ŀ¼��
 * \param lpThreadParameter δʹ�ô˲���
 */
DWORD __stdcall MergeDump(LPVOID lpThreadParameter)
{
    DWORD start_addr = 0;
    DWORD end_addr = 0;
    if (_Getlong("Dump�ڴ����ʼλ��", &start_addr, 4, '0', DIA_HEXONLY) != 0)
    {
        return -1;
    }
    if (_Getlong("Dump�ڴ�Ľ���λ��", &end_addr, 4, '0', DIA_HEXONLY) != 0)
    {
        return -1;
    }
    const DWORD size = end_addr - start_addr;
    BYTE* buf = new BYTE[size];
    memset(buf, 0, size);
    t_table* memory_table = (t_table*)_Plugingetvalue(VAL_MEMORY);
    t_sorted memory_data = memory_table->data;
    for (int i = 0; i < memory_data.n; i++)
    {
        t_memory* memory = (t_memory*)_Getsortedbyselection(&memory_data, i);
        if (memory->base >= start_addr && memory->base < end_addr)
        {
            _Readmemory(buf + memory->base - start_addr, memory->base, memory->size, MM_SILENT);
        }
    }
    char current_dir[MAX_PATH];
    HANDLE hProcess = (HANDLE)_Plugingetvalue(VAL_HPROCESS);
    GetModuleFileNameExA(hProcess, NULL, current_dir, MAX_PATH);
    PathRemoveFileSpecA(current_dir);
    //bug: OD api�����޷���ȡ��ǰĿ¼
    //char* current_dir = (char*)_Plugingetvalue(VAL_CURRENTDIR);
    char file_path[MAX_PATH] = { 0 };
    sprintf_s(file_path, 256, "%s\\Dump_%08X_%08X.bin", current_dir, start_addr, end_addr);
    HANDLE hfile = CreateFileA(file_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hfile == INVALID_HANDLE_VALUE)
    {
        _Addtolist(0, 1, "����Dump�ļ� %s ʧ�ܣ�%s", file_path, GetLastErrorStr().c_str());
    }
    DWORD lpNumberOfBytesWritten = 0;
    if (!WriteFile(hfile, buf, size, &lpNumberOfBytesWritten, NULL))
    {
        _Addtolist(0, 1, "д��Dump�ļ� %s ʧ�ܣ�%s", file_path, GetLastErrorStr().c_str());
    }
    CloseHandle(hfile);
    _Addtolist(0, -1, "Dump��� �ļ�·����%s", file_path);
    delete[] buf;
    return 0;
}

extc int ODBG_Pausedex(int reason, int extdata, t_reg* reg, DEBUG_EVENT* debugevent)
{
    if (is_tracing)
    {
        if (reason == PP_SINGLESTEP || reason == PP_EVENT)
        {
            _Animate(ANIMATE_OFF);
            is_tracing = false;
            char name[TEXTLEN];
            if (_Findsymbolicname(reg->ip, name) > 1)
            {
                //����0��ʾû�ҵ�������1��ʾΪ�գ�����������ʾ���Ƴ���
                _Flash("[moye] ����api -> %#08x %s", reg->ip, name);
                _Addtolist(reg->ip, 1, "[moye] ����api -> %#08x %s", reg->ip, name);
            }
            else
            {
                _Flash("[moye] ���������� -> %#08x", reg->ip);
                _Addtolist(reg->ip, 1, "[moye] ���������� -> %#08x", reg->ip);
            }
            return 0;
        }
    }
    return 0;
}


/**
 * \brief ʹ��trace�ķ�ʽ������api����������
 */
void TraceToApi()
{
    _Deleteruntrace();
    is_tracing = true;
    ulong threadid = _Getcputhreadid();
    t_thread* thread = _Findthread(threadid);
    t_reg* reg = &thread->reg;
    t_memory* section = _Findmemory(reg->ip);
    //ODĬ��ѡ��ᵼ�¸��ٲ���ϵͳDll
    //ö������ģ�飬���ҽ����Ǳ��Ϊ��ϵͳDll
    t_table* module_table = (t_table*)_Plugingetvalue(VAL_MODULES);
    t_sorted module_data = module_table->data;
    for (int i = 0; i < module_data.n; ++i)
    {
        t_module* module = (t_module*)_Getsortedbyselection(&module_data, i);
        module->issystemdll = false;
    }
    _Animate(ANIMATE_TRIN);
    //_Settracepauseoncommands((char*)"call CONST");
    //_Settracecount();
    _Settracecondition((char*)"0", 0, 0, 0, section->base, section->base + section->size);
    _Startruntrace(reg);
    _Go(threadid, 0, STEP_RUN, 1, 0);
}

/**
 * \brief ����ģ�����з�����fs�Ĵ�����ָ�unicorn��bug��
 * \param emu
 * \param user_data
 * \return
 */
 // bool EmuFsSolver(Emulator* emu, void* user_data)
 // {
 //     BYTE code[16] = {};
 //     DWORD addr = 0;
 //     DWORD value = 0;
 //     _Readmemory(code, emu->regs.eip, 16, MM_SILENT);
 //     a.Disasm(emu->regs.eip,code,sizeof(code));
 //     switch(a.vec_insn[0]->id)
 //     {
 //         case X86_INS_PUSH:
 //             if (a.vec_insn[0]->detail->x86.operands[0].mem.segment == X86_REG_FS)
 //             {
 //                 emu->regs.esp -= a.vec_insn[0]->detail->x86.operands[0].size;
 //                 addr = emu->regs.fs_base + emu->GetMemAddr(a.vec_insn[0]->detail->x86.operands[0].mem);
 //                 emu->ReadMemory(addr, a.vec_insn[0]->detail->x86.operands[0].size, &value);
 //                 emu->WriteMemory(emu->regs.esp, a.vec_insn[0]->detail->x86.operands[0].size, &value);
 //                 emu->regs.eip += a.vec_insn[0]->size;
 //             }
 //             break;
 //         case X86_INS_POP:
 //             if(a.vec_insn[0]->detail->x86.operands[0].type==X86_OP_MEM)
 //             {
 //                 if(a.vec_insn[0]->detail->x86.operands[0].mem.segment==X86_REG_FS)
 //                 {
 //                     addr = emu->regs.fs_base + emu->GetMemAddr(a.vec_insn[0]->detail->x86.operands[0].mem);
 //                     emu->ReadMemory(emu->regs.esp,a.vec_insn[0]->detail->x86.operands[0].size,&value);
 //                     emu->WriteMemory(addr,a.vec_insn[0]->detail->x86.operands[0].size,&value);
 //                     emu->regs.esp+=a.vec_insn[0]->detail->x86.operands[0].size;
 //                     emu->regs.eip+=a.vec_insn[0]->size;
 //                 }
 //             }
 //             break;
 //         case X86_INS_MOV:
 //             if(a.vec_insn[0]->detail->x86.operands[0].type==X86_OP_MEM)
 //             {
 //                 if(a.vec_insn[0]->detail->x86.operands[0].mem.segment==X86_REG_FS)
 //                 {
 //                     if(a.vec_insn[0]->detail->x86.operands[1].type==X86_OP_IMM)
 //                     {
 //                         addr = emu->regs.fs_base + emu->GetMemAddr(a.vec_insn[0]->detail->x86.operands[0].mem);
 //                         emu->WriteMemory(addr, a.vec_insn[0]->detail->x86.operands[1].size, &a.vec_insn[0]->detail->x86.operands[1].imm);
 //                         emu->regs.eip += a.vec_insn[0]->size;
 //                     }
 //                     else if(a.vec_insn[0]->detail->x86.operands[1].type==X86_OP_REG)
 //                     {
 //                         addr = emu->regs.fs_base + emu->GetMemAddr(a.vec_insn[0]->detail->x86.operands[0].mem);
 //                         value = emu->GetReg(a.vec_insn[0]->detail->x86.operands[1].reg);
 //                         emu->WriteMemory(addr, a.vec_insn[0]->detail->x86.operands[1].size, &value);
 //                         emu->regs.eip += a.vec_insn[0]->size;
 //                     }
 //                     else
 //                     {
 //                         _Addtolist(emu->regs.eip,1,"%#08x �޷������fs�μĴ�������",emu->regs.eip);
 //                     }
 //                 }
 //             }
 //             else if(a.vec_insn[0]->detail->x86.operands[1].type==X86_OP_MEM)
 //             {
 //                 if(a.vec_insn[0]->detail->x86.operands[1].mem.segment==X86_REG_FS)
 //                 {
 //                     if(a.vec_insn[0]->detail->x86.operands[0].type==X86_OP_REG)
 //                     {
 //                         addr = emu->regs.fs_base + emu->GetMemAddr(a.vec_insn[0]->detail->x86.operands[1].mem);
 //                         emu->ReadMemory(addr, a.vec_insn[0]->detail->x86.operands[1].size, &value);
 //                         emu->SetReg(a.vec_insn[0]->detail->x86.operands[0].reg,value);
 //                         emu->regs.eip += a.vec_insn[0]->size;
 //                     }
 //                     else
 //                     {
 //                         _Addtolist(emu->regs.eip,1,"%#08x �޷������fs�μĴ�������",emu->regs.eip);
 //                     }
 //                 }
 //             }
 //             break;
 //         default:
 //             for (DWORD i = 0; i < a.vec_insn[0]->detail->x86.op_count; i++)
 //             {
 //                 if (a.vec_insn[0]->detail->x86.operands[i].type == X86_OP_MEM&&
 //                     a.vec_insn[0]->detail->x86.operands[i].mem.segment == X86_REG_FS)
 //                 {
 //                     _Addtolist(emu->regs.eip,1,"%#08x �޷������fs�μĴ�������",emu->regs.eip);
 //                     break;
 //                 }
 //             }
 //             break;
 //     }
 //     return true;
 // }

/**
 * \brief ����ģ�����е�jmpָ�unicorn��bug��
 * \param emu
 * \param user_data
 * \return
 */
bool EmuJmpImmSolver(Emulator* emu, void* user_data)
{
    BYTE code[16];
    emu->ReadMemory(emu->regs.eip, sizeof(code), code);
    if(code[0]==0xE9)
    {
        DWORD addr = emu->regs.eip + 5 + *(DWORD*)(code + 1);
        emu->regs.eip = addr;
    }
    return true;
}

/**
 * \brief ����ģ������������cpuid��rdtscָ��
 * \param emu 
 * \param user_data 
 * \return 
 */
bool EmuSpecialInsSolverCallback(Emulator* emu, void* user_data)
{
    BYTE code[16];
    emu->ReadMemory(emu->regs.eip, sizeof(code), code);
    a.Clear();
    a.Disasm(emu->regs.eip, code, sizeof(code));
    if (a.count != 1) return true;
    if(a.vec_insn[0]->id==X86_INS_CPUID)
    {
        _Addtolist(emu->regs.eip,0,"%#08x hook cpuid",emu->regs.eip);
        emu->regs.eax = cpuid_eax;
        emu->regs.ecx = cpuid_ecx;
        emu->regs.edx = cpuid_edx;
        emu->regs.ebx = cpuid_ebx;
        emu->regs.eip+=a.vec_insn[0]->size;
    }
    // else if(a.vec_insn[0]->id==X86_INS_RDTSC)
    // {
    //     _Addtolist(emu->regs.eip,0,"%#08x hook rdtsc",emu->regs.eip);
    //     ++rdtsc_eax;
    //     if(rdtsc_eax==0)
    //     {
    //         ++rdtsc_edx;
    //     }
    //     emu->regs.eax = rdtsc_eax;
    //     emu->regs.edx = rdtsc_edx;
    //     emu->regs.eip+=a.vec_insn[0]->size;
    // }
    return true;
}

 /**
  * \brief EmuToApi����ʹ�õĻص�����
  * \param emu ģ����thisָ��
  * \param user_data ���eip���ڵ��ڴ��
  * \return �����棬����ֹ�����ص�����ִ��
  */
bool EmuToApiCallback(Emulator* emu, void* user_data)
{
    t_memory* mem_code = (t_memory*)user_data;
    if (!emu_control)
    {
        _Addtolist(0, 1, "�ֶ��ж�ģ��");
        emu->Stop();
        return true;
    }
    if (emu->regs.eip < mem_code->base || emu->regs.eip >= mem_code->base + mem_code->size)
    {
        char name[TEXTLEN] = {};
        if (_Findsymbolicname(emu->regs.eip, name) > 1)
        {
            //����0��ʾû�ҵ�������1��ʾΪ�գ�����������ʾ���Ƴ���
            _Flash("[moye] ģ����api -> 0x%08x %s", emu->regs.eip, name);
            _Addtolist(emu->regs.eip, 1, "[moye] ģ����api -> 0x%08x %s", emu->regs.eip, name);
        }
        else
        {
            _Flash("[moye] ģ���������� -> 0x%08x", emu->regs.eip);
            _Addtolist(emu->regs.eip, 1, "[moye] ģ���������� -> 0x%08x", emu->regs.eip);
        }
        emu->Stop();
    }
    return true;
}



/**
 * \brief ģ����api
 * \param lpThreadParameter δʹ�ô˲���
 * \return 0
 */
DWORD __stdcall EmuToApi(LPVOID lpThreadParameter)
{
    emu_control = true;
    Emulator emu;
    _Infoline("ģ��ִ����...");
    emu.MapMemoryFromOD();
    emu.SetRegFromOD();
    if (special_ins_solver_control)
    {
        emu.RegisterCallback(EmuSpecialInsSolverCallback, NULL);
    }
    //emu.RegisterCallback(EmuJmpImmSolver, NULL);       //bug unicorn��bug����jmpָ����ܻ����
    emu.RegisterCallback(EmuToApiCallback, _Findmemory(emu.regs.eip));
    DWORD count = emu.Run();
    _Flash("ģ������ֹ");
    emu.LogError();
    emu.LogEnvironment();
    _Addtolist(0, -1, "��ģ����%lu��ָ��", count);
    _Setcpu(0, emu.regs.eip, 0, 0, CPU_NOFOCUS);
    return 0;
}


/*
 *\brief FixSpIATʹ�õĻص�
 */
bool FixSpIATCallback(Emulator* emu, void* user_data)
{
    t_memory* mem_svmp1 = (t_memory*)user_data;
    if (emu->regs.eip < mem_svmp1->base || emu->regs.eip >= mem_svmp1->base + mem_svmp1->size)
    {
        emu->Stop();
    }
    return true;
}

/*
 *\brief ��svmp1�������������޸�IAT
 */
DWORD __stdcall FixSpIAT(LPVOID lpThreadParameter)
{
    //----------------------��ʼ�����ݣ�Ѱ�Ҵ���ں�.svmp1 ������---------------------
    HANDLE hprocess = (HANDLE)_Plugingetvalue(VAL_HPROCESS);
    t_thread* thread = _Findthread(_Getcputhreadid());
    t_memory* mem_code = _Findmemory(0x00401000);
    if (mem_code == nullptr)
    {
        MessageBoxA(0, "δ����0x401000������������", "����", MB_TOPMOST | MB_ICONWARNING | MB_OK);
        return 0;
    }
    t_memory* mem_svmp1 = {};
    t_table memory_table = *(t_table*)_Plugingetvalue(VAL_MEMORY);
    t_sorted memory_data = memory_table.data;
    for (int i = 0; i < memory_data.n; i++)
    {
        t_memory* memory = (t_memory*)_Getsortedbyselection(&memory_data, i);
        if (strcmp(memory->sect, ".svmp1") == 0)
        {
            mem_svmp1 = memory;
            goto next;
        }
    }
    MessageBoxA(0, "δ������������.svmp1", "����", MB_TOPMOST | MB_ICONWARNING | MB_OK);
    return 0;
next:
    DWORD tmp_iat_begin;
    if (_Getlong("��ʱ������ʼλ��", &tmp_iat_begin, 4, '0', DIA_HEXONLY) != 0)
    {
        return -1;
    }
    //-------------------------��ȡ����-------------------------------------
    SEG_MAP seg_map[2];
    //��ȡ.text��.svmp1�Ĵ���
    seg_map[0] = { mem_code->base,mem_code->size,"",new uchar[mem_code->size] };
    _Readmemory(seg_map[0].buf, mem_code->base, mem_code->size, MM_RESILENT);
    seg_map[1] = { mem_svmp1->base,mem_svmp1->size,"",new uchar[mem_svmp1->size] };
    _Readmemory(seg_map[1].buf, mem_svmp1->base, mem_svmp1->size, MM_RESILENT);
    //---------------------------��ʼ��ģ����--------------------------------
    emu_control = true;
    Emulator emu;
    emu.MapMemoryFromOD();
    emu.SetRegFromOD();
    emu.RegisterCallback(EmuJmpImmSolver, NULL);       //bug unicorn��bug����jmpָ����ܻ����
    emu.RegisterCallback(FixSpIATCallback, mem_svmp1);
    //---------------------��svmp1����������---------------------------------
    DWORD cnt_common_calljmp = 0;
    DWORD cnt_common_mov = 0;
    DWORD cnt_virtual_calljmp = 0;
    DWORD cnt_virtual_mov = 0;
    vector<api_info> vec_iat_data;	                     //��ŵȴ��ؽ���IAT����
    vector<pair<DWORD32, DWORD32>> vec_err_call;         //��һ������Ϊentry���ڶ�������Ϊʵ�ʵ���api�ĵ�ַ��δ֪�����쳣������Щ��ַ��Ҫ�ֶ�����
    char dll_path[MAX_PATH] = {};
    char api_name[TEXTLEN] = {};
    t_module* module;
    //entry=4cf731
    //eip=0x00d6cd7a
    for (DWORD i = mem_svmp1->base; i < mem_svmp1->base + mem_svmp1->size - 100; i++)
    {
        //����iat��������
        if ((seg_map[1].buf[i - mem_svmp1->base] == 0x9C || seg_map[1].buf[i - mem_svmp1->base + 1] == 0x9C))
        {
            a.Clear();
            a.Disasm(i, seg_map[1].buf + i - mem_svmp1->base, 48, 3);
            if (a.count != 3) continue;
            if (a.vec_insn[0]->id!=X86_INS_PUSHFD&&a.vec_insn[0]->id!=X86_INS_PUSH) continue;
            if (a.vec_insn[1]->id!=X86_INS_PUSHFD&&a.vec_insn[1]->id!=X86_INS_PUSH) continue;
            if (a.vec_insn[2]->id!=X86_INS_JMP) continue;
            const DWORD jmp_to = (DWORD)a.vec_insn[2]->detail->x86.operands[0].imm;       //��ת��Ŀ���ַ
            if (jmp_to < mem_svmp1->base || jmp_to >= mem_svmp1->base + mem_svmp1->size) continue;
            a.Clear();
            a.Disasm(jmp_to, seg_map[1].buf + jmp_to - mem_svmp1->base, 5, 1);
            if (a.count != 1) continue;
            if (a.vec_insn[0]->id != X86_INS_MOV) continue;
            if (a.vec_insn[0]->detail->x86.operands[0].type != X86_OP_REG) continue;
            if (a.vec_insn[0]->detail->x86.operands[1].type != X86_OP_IMM) continue;
            if (a.vec_insn[0]->detail->x86.operands[1].imm != 0) continue;

            //��ʼģ��ֱ��eip����svmp1����
            //[esp]��0x00400000��Ϊ���ж��Ƿ���mov����
            //(�����Ĵ�����0��Ϊ���ж�mov����ʱ�޸ĵ����ĸ��Ĵ���)
            DWORD v = 0x00400000;
            emu.WriteMemory(emu.regs.esp, 4, &v);
            emu.regs.eax = 0;
            emu.regs.ecx = 0;
            emu.regs.edx = 0;
            emu.regs.ebx = 0;
            emu.regs.esp = thread->reg.r[REG_ESP];
            emu.regs.ebp = 0;
            emu.regs.esi = 0;
            emu.regs.edi = 0;
            emu.regs.eip = i;
            emu.regs.efl = 0x246;
            emu.Run();
            //�ж�eip�Ƿ�Ϊ0x00401000���������Ϊmov reg��[mem]��ʽ������Ϊcall��jmp����
            if (emu.regs.eip == 0x00400000)
            {
                //mov reg, dword ptr [mem]
                if (emu.regs.eax != 0 && _Findsymbolicname(emu.regs.eax, api_name) > 1)
                {
                    module = _Findmodule(emu.regs.eax);
                    if (module == NULL)
                    {
                        _Addtolist(i, 1, "[moye] �޷���λ��ģ�� entry = %#08x api -> %#08x", i, emu.regs.eax);
                        vec_err_call.emplace_back(i, emu.regs.eax);
                        continue;
                    }
                    strcpy_s(dll_path, module->name);
                    vec_iat_data.push_back({ dll_path, api_name, i, emu.regs.eax, 3 });
                    _Addtolist(i, 0, "[moye] |һ��|����Ϊ mov entry = %#08x api -> %#08x %s", i, emu.regs.eax, api_name);
                    ++cnt_common_mov;
                }
                else if (emu.regs.ecx != 0 && _Findsymbolicname(emu.regs.ecx, api_name) > 1)
                {
                    module = _Findmodule(emu.regs.ecx);
                    if (module == NULL)
                    {
                        _Addtolist(i, 1, "[moye] �޷���λ��ģ�� entry = %#08x api -> %#08x", i, emu.regs.ecx);
                        vec_err_call.emplace_back(i, emu.regs.ecx);
                        continue;
                    }
                    strcpy_s(dll_path, module->name);
                    vec_iat_data.push_back({ dll_path, api_name, i, emu.regs.ecx, 4 });
                    _Addtolist(i, 0, "[moye] |һ��|����Ϊ mov entry = %#08x api -> %#08x %s", i, emu.regs.ecx, api_name);
                    ++cnt_common_mov;
                }
                else if (emu.regs.edx != 0 && _Findsymbolicname(emu.regs.edx, api_name) > 1)
                {
                    module = _Findmodule(emu.regs.edx);
                    if (module == NULL)
                    {
                        _Addtolist(i, 1, "[moye] �޷���λ��ģ�� entry = %#08x api -> %#08x", i, emu.regs.edx);
                        vec_err_call.emplace_back(i, emu.regs.edx);
                        continue;
                    }
                    strcpy_s(dll_path, module->name);
                    vec_iat_data.push_back({ dll_path, api_name, i, emu.regs.edx, 5 });
                    _Addtolist(i, 0, "[moye] |һ��|����Ϊ mov entry = %#08x api -> %#08x %s", i, emu.regs.edx, api_name);
                    ++cnt_common_mov;
                }
                else if (emu.regs.ebx != 0 && _Findsymbolicname(emu.regs.ebx, api_name) > 1)
                {
                    module = _Findmodule(emu.regs.ebx);
                    if (module == NULL)
                    {
                        _Addtolist(i, 1, "[moye] �޷���λ��ģ�� entry = %#08x api -> %#08x", i, emu.regs.ebx);
                        vec_err_call.emplace_back(i, emu.regs.ebx);
                        continue;
                    }
                    strcpy_s(dll_path, module->name);
                    vec_iat_data.push_back({ dll_path, api_name, i, emu.regs.ebx, 6 });
                    _Addtolist(i, 0, "[moye] |һ��|����Ϊ mov entry = %#08x api -> %#08x %s", i, emu.regs.ebx, api_name);
                    ++cnt_common_mov;
                }
                else if (emu.regs.ebp != 0 && _Findsymbolicname(emu.regs.ebp, api_name) > 1)
                {
                    module = _Findmodule(emu.regs.ebp);
                    if (module == NULL)
                    {
                        _Addtolist(i, 1, "[moye] �޷���λ��ģ�� entry = %#08x api -> %#08x", i, emu.regs.ebp);
                        vec_err_call.emplace_back(i, emu.regs.ebp);
                        continue;
                    }
                    strcpy_s(dll_path, module->name);
                    vec_iat_data.push_back({ dll_path, api_name, i, emu.regs.ebp, 7 });
                    _Addtolist(i, 0, "[moye] |һ��|����Ϊ mov entry = %#08x api -> %#08x %s", i, emu.regs.ebp, api_name);
                    ++cnt_common_mov;
                }
                else if (emu.regs.esi != 0 && _Findsymbolicname(emu.regs.esi, api_name) > 1)
                {
                    module = _Findmodule(emu.regs.esi);
                    if (module == NULL)
                    {
                        _Addtolist(i, 1, "[moye] �޷���λ��ģ�� entry = %#08x api -> %#08x", i, emu.regs.esi);
                        vec_err_call.emplace_back(i, emu.regs.esi);
                        continue;
                    }
                    strcpy_s(dll_path, module->name);
                    vec_iat_data.push_back({ dll_path, api_name, i, emu.regs.esi, 8 });
                    _Addtolist(i, 0, "[moye] |һ��|����Ϊ mov entry = %#08x api -> %#08x %s", i, emu.regs.esi, api_name);
                    ++cnt_common_mov;
                }
                else if (emu.regs.edi != 0 && _Findsymbolicname(emu.regs.edi, api_name) > 1)
                {
                    module = _Findmodule(emu.regs.edi);
                    if (module == NULL)
                    {
                        _Addtolist(i, 1, "[moye] �޷���λ��ģ�� entry = %#08x api -> %#08x", i, emu.regs.edi);
                        vec_err_call.emplace_back(i, emu.regs.edi);
                        continue;
                    }
                    strcpy_s(dll_path, module->name);
                    vec_iat_data.push_back({ dll_path, api_name, i, emu.regs.edi, 9 });
                    _Addtolist(i, 0, "[moye] |һ��|����Ϊ mov entry = %#08x api -> %#08x %s", i, emu.regs.edi, api_name);
                    ++cnt_common_mov;
                }
                else
                {
                    _Addtolist(i, 1, "[moye] δ֪�ļ��ܷ�ʽ entry = %#08x api -> %#08x", i, emu.regs.eip);
                    vec_err_call.emplace_back(i, emu.regs.eip);
                    continue;
                }
            }
            else if (_Findsymbolicname(emu.regs.eip, api_name) > 1)
            {
                //����api����ģ����
                module = _Findmodule(emu.regs.eip);
                if (module == NULL)
                {
                    _Addtolist(i, 1, "[moye] �޷���λ��ģ�� entry = %#08x api -> %#08x", i, emu.regs.eip);
                    vec_err_call.emplace_back(i, emu.regs.eip);
                    continue;
                }
                strcpy_s(dll_path, module->name);
                //jmp���ܱ�call����ֻ��һ��lea esp, [esp + 4]������Ҫ���ж���call����jmp
                if (thread->reg.r[REG_ESP] == emu.regs.esp)
                {
                    //jmp dword ptr [mem]
                    ++cnt_common_calljmp;
                    vec_iat_data.push_back({ dll_path, api_name, i, emu.regs.eip, 1 });
                    _Addtolist(i, 0, "[moye] |һ��|�ڴ˴��޸� entry = %#08x api -> %#08x %s", i, emu.regs.eip, api_name);
                }
                else
                {
                    _Addtolist(i, 1, "[moye] δ֪�ļ��ܷ�ʽ entry = %#08x api -> %#08x", i, emu.regs.eip);
                    vec_err_call.emplace_back(i, emu.regs.eip);
                    continue;
                }
            }
            else
            {
                _Addtolist(i, 1, "[moye] δ֪�ļ��ܷ�ʽ entry = %#08x api -> %#08x", i, emu.regs.eip);
                vec_err_call.emplace_back(i, emu.regs.eip);
                continue;
            }
            _Infoline("[moye] |һ��|������ɣ�entry -> %#08X api -> %-20s", i, api_name);
            //_Setcpu(0, i, 0, 0, CPU_NOFOCUS);
            continue;
        }
        //���⻯iat��������
        else if (seg_map[1].buf[i - mem_svmp1->base + 2] == 0x9C && seg_map[1].buf[i - mem_svmp1->base + 4] == 0 &&
            seg_map[1].buf[i - mem_svmp1->base + 5] == 0 && seg_map[1].buf[i - mem_svmp1->base + 6] == 0 && seg_map[1].buf[i - mem_svmp1->base + 7] == 0)
        {
            a.Clear();
            a.Disasm(i, seg_map[1].buf + i - mem_svmp1->base, 26, 7);
            if (a.count != 7) continue;
            if (a.vec_insn[0]->id != X86_INS_PUSH) continue;
            if (a.vec_insn[1]->id != X86_INS_PUSH) continue;
            if (a.vec_insn[2]->id != X86_INS_PUSHFD) continue;
            if (a.vec_insn[3]->id != X86_INS_MOV) continue;
            if (a.vec_insn[3]->detail->x86.operands[0].type != X86_OP_REG) continue;
            if (a.vec_insn[3]->detail->x86.operands[1].type != X86_OP_IMM) continue;
            if (a.vec_insn[3]->detail->x86.operands[1].imm != 0) continue;
            if (a.vec_insn[4]->id != X86_INS_LEA) continue;
            if (a.vec_insn[5]->id != X86_INS_LEA) continue;
            if (a.vec_insn[6]->id != X86_INS_LEA) continue;

            //��ʼģ��ֱ��eip����svmp1����
            //[esp]��0x00400000��Ϊ���ж��Ƿ���mov����
            //(�����Ĵ�����0��Ϊ���ж�mov����ʱ�޸ĵ����ĸ��Ĵ���)
            DWORD v = 0x00400000;
            emu.WriteMemory(emu.regs.esp, 4, &v);
            emu.regs.eax = 0;
            emu.regs.ecx = 0;
            emu.regs.edx = 0;
            emu.regs.ebx = 0;
            emu.regs.esp = thread->reg.r[REG_ESP];
            emu.regs.ebp = 0;
            emu.regs.esi = 0;
            emu.regs.edi = 0;
            emu.regs.eip = i;
            emu.regs.efl = 0x246;
            emu.Run();
            //�ж�eip�Ƿ�Ϊ0x00401000���������Ϊmov reg��[mem]��ʽ������Ϊcall��jmp����
            if (emu.regs.eip == 0x00400000)
            {
                //mov reg, dword ptr [mem]
                if (emu.regs.eax != 0 && _Findsymbolicname(emu.regs.eax, api_name) > 1)
                {
                    module = _Findmodule(emu.regs.eax);
                    if (module == NULL)
                    {
                        _Addtolist(i, 1, "[moye] �޷���λ��ģ�� entry = %#08x api -> %#08x", i, emu.regs.eax);
                        vec_err_call.emplace_back(i, emu.regs.eax);
                        continue;
                    }
                    strcpy_s(dll_path, module->name);
                    vec_iat_data.push_back({ dll_path, api_name, i, emu.regs.eax, 3 });
                    _Addtolist(i, 0, "[moye] |����|����Ϊ mov entry = %#08x api -> %#08x %s", i, emu.regs.eax, api_name);
                    ++cnt_virtual_mov;
                }
                else if (emu.regs.ecx != 0 && _Findsymbolicname(emu.regs.ecx, api_name) > 1)
                {
                    module = _Findmodule(emu.regs.ecx);
                    if (module == NULL)
                    {
                        _Addtolist(i, 1, "[moye] �޷���λ��ģ�� entry = %#08x api -> %#08x", i, emu.regs.ecx);
                        vec_err_call.emplace_back(i, emu.regs.ecx);
                        continue;
                    }
                    strcpy_s(dll_path, module->name);
                    vec_iat_data.push_back({ dll_path, api_name, i, emu.regs.ecx, 4 });
                    _Addtolist(i, 0, "[moye] |����|����Ϊ mov entry = %#08x api -> %#08x %s", i, emu.regs.ecx, api_name);
                    ++cnt_virtual_mov;
                }
                else if (emu.regs.edx != 0 && _Findsymbolicname(emu.regs.edx, api_name) > 1)
                {
                    module = _Findmodule(emu.regs.edx);
                    if (module == NULL)
                    {
                        _Addtolist(i, 1, "[moye] �޷���λ��ģ�� entry = %#08x api -> %#08x", i, emu.regs.edx);
                        vec_err_call.emplace_back(i, emu.regs.edx);
                        continue;
                    }
                    strcpy_s(dll_path, module->name);
                    vec_iat_data.push_back({ dll_path, api_name, i, emu.regs.edx, 5 });
                    _Addtolist(i, 0, "[moye] |����|����Ϊ mov entry = %#08x api -> %#08x %s", i, emu.regs.edx, api_name);
                    ++cnt_virtual_mov;
                }
                else if (emu.regs.ebx != 0 && _Findsymbolicname(emu.regs.ebx, api_name) > 1)
                {
                    module = _Findmodule(emu.regs.ebx);
                    if (module == NULL)
                    {
                        _Addtolist(i, 1, "[moye] �޷���λ��ģ�� entry = %#08x api -> %#08x", i, emu.regs.ebx);
                        vec_err_call.emplace_back(i, emu.regs.ebx);
                        continue;
                    }
                    strcpy_s(dll_path, module->name);
                    vec_iat_data.push_back({ dll_path, api_name, i, emu.regs.ebx, 6 });
                    _Addtolist(i, 0, "[moye] |����|����Ϊ mov entry = %#08x api -> %#08x %s", i, emu.regs.ebx, api_name);
                    ++cnt_virtual_mov;
                }
                else if (emu.regs.ebp != 0 && _Findsymbolicname(emu.regs.ebp, api_name) > 1)
                {
                    module = _Findmodule(emu.regs.ebp);
                    if (module == NULL)
                    {
                        _Addtolist(i, 1, "[moye] �޷���λ��ģ�� entry = %#08x api -> %#08x", i, emu.regs.ebp);
                        vec_err_call.emplace_back(i, emu.regs.ebp);
                        continue;
                    }
                    strcpy_s(dll_path, module->name);
                    vec_iat_data.push_back({ dll_path, api_name, i, emu.regs.ebp, 7 });
                    _Addtolist(i, 0, "[moye] |����|����Ϊ mov entry = %#08x api -> %#08x %s", i, emu.regs.ebp, api_name);
                    ++cnt_virtual_mov;
                }
                else if (emu.regs.esi != 0 && _Findsymbolicname(emu.regs.esi, api_name) > 1)
                {
                    module = _Findmodule(emu.regs.esi);
                    if (module == NULL)
                    {
                        _Addtolist(i, 1, "[moye] �޷���λ��ģ�� entry = %#08x api -> %#08x", i, emu.regs.esi);
                        vec_err_call.emplace_back(i, emu.regs.esi);
                        continue;
                    }
                    strcpy_s(dll_path, module->name);
                    vec_iat_data.push_back({ dll_path, api_name, i, emu.regs.esi, 8 });
                    _Addtolist(i, 0, "[moye] |����|����Ϊ mov entry = %#08x api -> %#08x %s", i, emu.regs.esi, api_name);
                    ++cnt_virtual_mov;
                }
                else if (emu.regs.edi != 0 && _Findsymbolicname(emu.regs.edi, api_name) > 1)
                {
                    module = _Findmodule(emu.regs.edi);
                    if (module == NULL)
                    {
                        _Addtolist(i, 1, "[moye] �޷���λ��ģ�� entry = %#08x api -> %#08x", i, emu.regs.edi);
                        vec_err_call.emplace_back(i, emu.regs.edi);
                        continue;
                    }
                    strcpy_s(dll_path, module->name);
                    vec_iat_data.push_back({ dll_path, api_name, i, emu.regs.edi, 9 });
                    _Addtolist(i, 0, "[moye] |����|����Ϊ mov entry = %#08x api -> %#08x %s", i, emu.regs.edi, api_name);
                    ++cnt_virtual_mov;
                }
                else
                {
                    _Addtolist(i, 1, "[moye] δ֪�ļ��ܷ�ʽ entry = %#08x api -> %#08x", i, emu.regs.eip);
                    vec_err_call.emplace_back(i, emu.regs.eip);
                    continue;
                }
            }
            else if (_Findsymbolicname(emu.regs.eip, api_name) > 1)
            {
                //����api����ģ����
                module = _Findmodule(emu.regs.eip);
                if (module == NULL)
                {
                    _Addtolist(i, 1, "[moye] �޷���λ��ģ�� entry = %#08x api -> %#08x", i, emu.regs.eip);
                    vec_err_call.emplace_back(i, emu.regs.eip);
                    continue;
                }
                strcpy_s(dll_path, module->name);
                //jmp���ܱ�call����ֻ��һ��lea esp, [esp + 4]������Ҫ���ж���call����jmp
                if (thread->reg.r[REG_ESP] == emu.regs.esp)
                {
                    //jmp dword ptr [mem]
                    ++cnt_virtual_calljmp;
                    vec_iat_data.push_back({ dll_path, api_name, i, emu.regs.eip, 1 });
                    _Addtolist(i, 0, "[moye] |����|�ڴ˴��޸� entry = %#08x api -> %#08x %s", i, emu.regs.eip, api_name);
                }
                else
                {
                    _Addtolist(i, 1, "[moye] δ֪�ļ��ܷ�ʽ entry = %#08x api -> %#08x", i, emu.regs.eip);
                    vec_err_call.emplace_back(i, emu.regs.eip);
                    continue;
                }
            }
            else
            {
                _Addtolist(i, 1, "[moye] δ֪�ļ��ܷ�ʽ entry = %#08x api -> %#08x", i, emu.regs.eip);
                vec_err_call.emplace_back(i, emu.regs.eip);
                continue;
            }
            _Infoline("[moye] |����|������ɣ�entry -> %#08X api -> %-20s", i, api_name);
            //_Setcpu(0, i, 0, 0, CPU_NOFOCUS);
            continue;
        }
    }

    delete[] seg_map[0].buf;
    delete[] seg_map[1].buf;
    //-----------------------�ؽ�iat��------------------------
    if (vec_iat_data.empty())
    {
        MessageBoxA(0, "δ���ҵ���Ҫ�޸��ĵط�", "��ʾ", MB_TOPMOST | MB_ICONWARNING | MB_OK);
        return 0;
    }
    sort(vec_iat_data.begin(), vec_iat_data.end());     //����

    DWORD tmp_addr = tmp_iat_begin;    //��ǰapi��ŵĵ�ַ
    char tmp[50] = {};                 //����ַ���������
    char errtext[TEXTLEN];
    t_asmmodel asmmodel;
    vec_iat_data.push_back({});   //ĩβ��ǣ����ҷ�Խ��
    for (DWORD i = 0; i < vec_iat_data.size() - 1; i++)
    {
        _Progress(i * 1000 / (vec_iat_data.size() - 1), (char*)"�ؽ�IAT����...����");
        switch (vec_iat_data[i].type)
        {
            case 1:
                _Writememory(&vec_iat_data[i].api_addr, tmp_addr, 4, MM_SILENT);
                _Writememory((char*)"\xFF\x25", vec_iat_data[i].fix_addr, 2, MM_SILENT);
                _Writememory(&tmp_addr, vec_iat_data[i].fix_addr + 2, 4, MM_SILENT);
                break;
                //�������޸���ʽ�£�����Ҫcall���͵��޸�
                // case 2:
                //     _Writememory(&vec_iat_data[i].api_addr, tmp_addr, 4, MM_SILENT);
                //     _Writememory((char*)"\x58\xFF\x25", vec_iat_data[i].fix_addr, 3, MM_SILENT);
                //     _Writememory(&tmp_addr, vec_iat_data[i].fix_addr + 3, 4, MM_SILENT);
                //     break;
            case 3:
                _Writememory(&vec_iat_data[i].api_addr, tmp_addr, 4, MM_SILENT);
                sprintf_s(tmp, "mov eax, dword ptr [%08X]", tmp_addr);
                _Assemble(tmp, vec_iat_data[i].fix_addr, &asmmodel, 0, 0, errtext);
                _Writememory(asmmodel.code, vec_iat_data[i].fix_addr, asmmodel.length, MM_SILENT);
                _Writememory((void*)"\xC3", vec_iat_data[i].fix_addr + 6, 1, MM_SILENT);
                break;
            case 4:
                _Writememory(&vec_iat_data[i].api_addr, tmp_addr, 4, MM_SILENT);
                sprintf_s(tmp, "mov ecx, dword ptr [%08X]", tmp_addr);
                _Assemble(tmp, vec_iat_data[i].fix_addr, &asmmodel, 0, 0, errtext);
                _Writememory(asmmodel.code, vec_iat_data[i].fix_addr, asmmodel.length, MM_SILENT);
                _Writememory((void*)"\xC3", vec_iat_data[i].fix_addr + 6, 1, MM_SILENT);
                break;
            case 5:
                _Writememory(&vec_iat_data[i].api_addr, tmp_addr, 4, MM_SILENT);
                sprintf_s(tmp, "mov edx, dword ptr [%08X]", tmp_addr);
                _Assemble(tmp, vec_iat_data[i].fix_addr, &asmmodel, 0, 0, errtext);
                _Writememory(asmmodel.code, vec_iat_data[i].fix_addr, asmmodel.length, MM_SILENT);
                _Writememory((void*)"\xC3", vec_iat_data[i].fix_addr + 6, 1, MM_SILENT);
                break;
            case 6:
                _Writememory(&vec_iat_data[i].api_addr, tmp_addr, 4, MM_SILENT);
                sprintf_s(tmp, "mov ebx, dword ptr [%08X]", tmp_addr);
                _Assemble(tmp, vec_iat_data[i].fix_addr, &asmmodel, 0, 0, errtext);
                _Writememory(asmmodel.code, vec_iat_data[i].fix_addr, asmmodel.length, MM_SILENT);
                _Writememory((void*)"\xC3", vec_iat_data[i].fix_addr + 6, 1, MM_SILENT);
                break;
            case 7:
                _Writememory(&vec_iat_data[i].api_addr, tmp_addr, 4, MM_SILENT);
                sprintf_s(tmp, "mov ebp, dword ptr [%08X]", tmp_addr);
                _Assemble(tmp, vec_iat_data[i].fix_addr, &asmmodel, 0, 0, errtext);
                _Writememory(asmmodel.code, vec_iat_data[i].fix_addr, asmmodel.length, MM_SILENT);
                _Writememory((void*)"\xC3", vec_iat_data[i].fix_addr + 6, 1, MM_SILENT);
                break;
            case 8:
                _Writememory(&vec_iat_data[i].api_addr, tmp_addr, 4, MM_SILENT);
                sprintf_s(tmp, "mov esi, dword ptr [%08X]", tmp_addr);
                _Assemble(tmp, vec_iat_data[i].fix_addr, &asmmodel, 0, 0, errtext);
                _Writememory(asmmodel.code, vec_iat_data[i].fix_addr, asmmodel.length, MM_SILENT);
                _Writememory((void*)"\xC3", vec_iat_data[i].fix_addr + 6, 1, MM_SILENT);
                break;
            case 9:
                _Writememory(&vec_iat_data[i].api_addr, tmp_addr, 4, MM_SILENT);
                sprintf_s(tmp, "mov edi, dword ptr [%08X]", tmp_addr);
                _Assemble(tmp, vec_iat_data[i].fix_addr, &asmmodel, 0, 0, errtext);
                _Writememory(asmmodel.code, vec_iat_data[i].fix_addr, asmmodel.length, MM_SILENT);
                _Writememory((void*)"\xC3", vec_iat_data[i].fix_addr + 6, 1, MM_SILENT);
                break;
            default:
                MessageBoxA(0, "�ؽ�IAT��ʱ�쳣������ȷ������", "����", MB_TOPMOST | MB_ICONERROR | MB_OK);
                break;
        }

        if (vec_iat_data[i].dll_name != vec_iat_data[i + 1].dll_name)
        {
            //��ͬdll�ĺ������ϣ���0����
            DWORD data = 0;
            tmp_addr += 4;
            _Writememory(&data, tmp_addr, 4, MM_SILENT);
            tmp_addr += 4;
        }
        else if (vec_iat_data[i].api_name != vec_iat_data[i + 1].api_name)
        {
            //���������ͬ�ĺ������������������µĺ�������Ҫ����4�ֽڷ����ĵ�ַ
            tmp_addr += 4;
        }
    }
    _Progress(0, 0);
    _Addtolist(0, 0, "[moye] --------------�޸����--------------");
    _Addtolist(0, 0, "[moye] һ������call/jmp��������%lu", cnt_common_calljmp);
    _Addtolist(0, 0, "[moye] һ������mov��������%lu", cnt_common_mov);
    _Addtolist(0, 0, "[moye] ��������call/jmp��������%lu", cnt_virtual_calljmp);
    _Addtolist(0, 0, "[moye] ��������mov��������%lu", cnt_virtual_mov);
    _Addtolist(0, 0, "[moye] ��ʱ���ݵ�ַ��0x%08x", tmp_iat_begin);
    _Addtolist(0, 0, "[moye] ��ʱ���ݴ�С��0x%08x", tmp_addr - tmp_iat_begin);
    _Addtolist(0, 0, "[moye] ------------------------------------");
    if (!vec_err_call.empty())
    {
        _Addtolist(0, 1, "[moye] --------����������Ҫ�ֶ�����--------");
        for (auto& i : vec_err_call)
        {
            _Addtolist(0, 1, "[moye] entry->0x%08x api->0x%08x", i.first, i.second);
        }
        _Addtolist(0, 1, "[moye] ------------------------------------");
    }
    _Flash("�޸����");
    return 0;
}


bool MemAccessAnalysisCallback(Emulator* emu, void* user_data)
{
    hash_data_addr = 0;
    hash_data_size = 0;
    BYTE code[16];
    emu->ReadMemory(emu->regs.eip, sizeof(code), code);
    a.Clear();
    a.Disasm(emu->regs.eip, code, sizeof(code));
    if (a.vec_insn[0]->id == X86_INS_CPUID)
    {
        _Message(emu->regs.eip, "[Special Insn] cpuid | 0x%08x %s %s", emu->regs.eip, a.vec_insn[0]->mnemonic, a.vec_insn[0]->op_str);
        a.Clear();
        return true;
    }
    if (a.vec_insn[0]->id == X86_INS_RDTSC)
    {
        _Message(emu->regs.eip, "[Special Insn] rdtsc | 0x%08x %s %s", emu->regs.eip, a.vec_insn[0]->mnemonic, a.vec_insn[0]->op_str);
        return true;
    }
    if (a.vec_insn[0]->id == X86_INS_IN)
    {
        _Message(emu->regs.eip, "[Special Insn] in | 0x%08x %s %s", emu->regs.eip, a.vec_insn[0]->mnemonic, a.vec_insn[0]->op_str);
        return true;
    }
    if (a.vec_insn[0]->id == X86_INS_OUT)
    {
        _Message(emu->regs.eip, "[Special Insn] out | 0x%08x %s %s", emu->regs.eip, a.vec_insn[0]->mnemonic, a.vec_insn[0]->op_str);
        return true;
    }
    if (a.vec_insn[0]->id == X86_INS_SGDT)
    {
        _Message(emu->regs.eip, "[Special Insn] sgdt | 0x%08x %s %s", emu->regs.eip, a.vec_insn[0]->mnemonic, a.vec_insn[0]->op_str);
        return true;
    }
    if (a.vec_insn[0]->id == X86_INS_SLDT)
    {
        _Message(emu->regs.eip, "[Special Insn] sldt | 0x%08x %s %s", emu->regs.eip, a.vec_insn[0]->mnemonic, a.vec_insn[0]->op_str);
        return true;
    }
    if (a.vec_insn[0]->id != X86_INS_LEA)
    {
        for (DWORD i = 0; i < a.vec_insn[0]->detail->x86.op_count; i++)
        {
            if (a.vec_insn[0]->detail->x86.operands[i].type == X86_OP_MEM)
            {
                DWORD mem_addr = emu->GetMemAddr(a.vec_insn[0]->detail->x86.operands[i].mem);
                DWORD value = 0;
                if (a.vec_insn[0]->detail->x86.operands[i].mem.segment == X86_REG_FS)
                {
                    DWORD offset = mem_addr;
                    mem_addr = emu->regs.fs_base + mem_addr;
                    emu->ReadMemory(mem_addr, 4, &value);
                    _Message(emu->regs.eip, "[ FS->[0x%02x] ] mem_addr = 0x%08x value = 0x%08x | 0x%08x %s %s", offset, mem_addr, value, emu->regs.eip, a.vec_insn[0]->mnemonic, a.vec_insn[0]->op_str);
                    break;
                }
                t_memory* memory = _Findmemory(mem_addr);
                if (memory == nullptr)
                {
                    _Addtolist(emu->regs.eip, 0, "�޷����ҵ�ָ���ڴ� mem_addr = 0x%08x 0x%08x %s %s", mem_addr, emu->regs.eip, a.vec_insn[0]->mnemonic, a.vec_insn[0]->op_str);
                    break;
                }
                //���˲���Ҫ��ע���ڴ���Ϣ
                if ((memory->type & TY_STACK) != 0) continue;
                if (memory->base < 0x200000) continue;
                emu->ReadMemory(mem_addr, a.vec_insn[0]->detail->x86.operands[i].size, &value);

                //��¼���ڴ��д�룬�����Ǳ�ʾ������Ƿ�˳��ִ�й���hash����
                if (strstr(memory->sect, ".svmp") != NULL)
                {
                    if (i == 0)
                    {
                        if (a.vec_insn[0]->id == X86_INS_POP)
                        {
                            _Message(emu->regs.eip, "[Write %s] mem_addr = 0x%08x value = 0x%08x | 0x%08x %s %s", memory->sect, mem_addr, value, emu->regs.eip, a.vec_insn[0]->mnemonic, a.vec_insn[0]->op_str);
                            hash_data_addr = mem_addr;
                            hash_data_size = a.vec_insn[0]->detail->x86.operands[i].size;
                            break;
                        }
                        if (a.vec_insn[0]->id == X86_INS_MOV)
                        {
                            _Message(emu->regs.eip, "[Write %s] mem_addr = 0x%08x value = 0x%08x | 0x%08x %s %s", memory->sect, mem_addr, value, emu->regs.eip, a.vec_insn[0]->mnemonic, a.vec_insn[0]->op_str);
                            hash_data_addr = mem_addr;
                            hash_data_size = a.vec_insn[0]->detail->x86.operands[i].size;
                            break;
                        }
                        if (a.vec_insn[0]->id == X86_INS_XCHG)
                        {
                            _Message(emu->regs.eip, "[Write %s] mem_addr = 0x%08x value = 0x%08x | 0x%08x %s %s", memory->sect, mem_addr, value, emu->regs.eip, a.vec_insn[0]->mnemonic, a.vec_insn[0]->op_str);
                            hash_data_addr = mem_addr;
                            hash_data_size = a.vec_insn[0]->detail->x86.operands[i].size;
                            break;
                        }
                    }
                    else if (i == 1)
                    {
                        if (a.vec_insn[0]->id == X86_INS_XCHG)
                        {
                            _Message(emu->regs.eip, "[Write %s] mem_addr = 0x%08x value = 0x%08x | 0x%08x %s %s", memory->sect, mem_addr, value, emu->regs.eip, a.vec_insn[0]->mnemonic, a.vec_insn[0]->op_str);
                            hash_data_addr = mem_addr;
                            hash_data_size = a.vec_insn[0]->detail->x86.operands[i].size;
                            break;
                        }
                    }
                    //if(strstr(memory->sect, ".svmp1") == NULL)
                    //_Message(emu->regs.eip, "[Read  %s] mem_addr = 0x%08x value = 0x%08x | 0x%08x %s %s", memory->sect, mem_addr, value, emu->regs.eip, a.vec_insn[0]->mnemonic, a.vec_insn[0]->op_str);
                }
                else if ((memory->type & TY_HEADER) != 0)
                {
                    _Message(emu->regs.eip, "[ PE  Header ] mem_addr = 0x%08x value = 0x%08x | 0x%08x %s %s", mem_addr, value, emu->regs.eip, a.vec_insn[0]->mnemonic, a.vec_insn[0]->op_str);
                }
                else if ((memory->type & TY_EXPDATA) != 0)
                {
                    _Message(emu->regs.eip, "[Export  Data] mem_addr = 0x%08x value = 0x%08x | 0x%08x %s %s", mem_addr, value, emu->regs.eip, a.vec_insn[0]->mnemonic, a.vec_insn[0]->op_str);
                }
                else if ((memory->type & TY_IMPDATA) != 0)
                {
                    _Message(emu->regs.eip, "[Import  Data] mem_addr = 0x%08x value = 0x%08x | 0x%08x %s %s", mem_addr, value, emu->regs.eip, a.vec_insn[0]->mnemonic, a.vec_insn[0]->op_str);
                }
                else if ((memory->type & TY_CODE) != 0)
                {
                    _Message(emu->regs.eip, "[Code section] mem_addr = 0x%08x value = 0x%08x | 0x%08x %s %s", mem_addr, value, emu->regs.eip, a.vec_insn[0]->mnemonic, a.vec_insn[0]->op_str);
                }
                else if ((memory->type & TY_RSRC) != 0)
                {
                    _Message(emu->regs.eip, "[Rsrc section] mem_addr = 0x%08x value = 0x%08x | 0x%08x %s %s", mem_addr, value, emu->regs.eip, a.vec_insn[0]->mnemonic, a.vec_insn[0]->op_str);
                }
                else if ((memory->type & TY_THREAD) != 0)
                {
                    _Message(emu->regs.eip, "[Thread  Data] mem_addr = 0x%08x value = 0x%08x | 0x%08x %s %s", mem_addr, value, emu->regs.eip, a.vec_insn[0]->mnemonic, a.vec_insn[0]->op_str);
                }
                else
                {
                    _Message(emu->regs.eip, "      -        mem_addr = 0x%08x value = 0x%08x | 0x%08x %s %s", mem_addr, value, emu->regs.eip, a.vec_insn[0]->mnemonic, a.vec_insn[0]->op_str);
                }
            }
        }
    }
    a.Clear();
    return true;
}

/**
 * \brief ����patch SP�����Hash���ݵĻص���SP�����������Hash�����Է�ֹ�����ִ�����̱��۸�
 * \param emu ģ����thisָ��
 * \param user_data �˻ص���ʹ�ô˶������
 * \return ��
 */
bool GetPatchSPWriteHashDataCallback(Emulator* emu, void* user_data)
{
    if (hash_data_addr)
    {
        DWORD hash_value = 0;
        char patch_asm[200];
        switch (hash_data_size)
        {
            case 1:
                emu->ReadMemory(hash_data_addr, 1, &hash_value);
                sprintf_s(patch_asm, "mov byte ptr [0x%08x], 0x%08x\n", hash_data_addr, hash_value);
                break;
            case 2:
                emu->ReadMemory(hash_data_addr, 2, &hash_value);
                sprintf_s(patch_asm, "mov word ptr [0x%08x], 0x%08x\n", hash_data_addr, hash_value);
                break;
            case 4:
                emu->ReadMemory(hash_data_addr, 4, &hash_value);
                sprintf_s(patch_asm, "mov dword ptr [0x%08x], 0x%08x\n", hash_data_addr, hash_value);
                break;
            default:
                _Addtolist(emu->regs.eip, 1, "Invalid hash_data_size %lu", hash_data_size);
                emu->Stop();
                return true;
        }
        OutputDebugStringA(patch_asm);
        hash_data_addr = 0;
        hash_data_size = 0;
    }
    return true;
}

/**
 * \brief Ѱ��SP��������ڵĻص�
 * \param emu ģ����thisָ��
 * \param user_data �˻ص���ʹ�ô˶������
 * \return ��
 */
bool FindSPVMExitCallback(Emulator* emu, void* user_data)
{
    BYTE code[32];
    emu->ReadMemory(emu->regs.eip, sizeof(code), code);
    a.Clear();
    a.Disasm(emu->regs.eip, code, sizeof(code), 2);
    if (a.count != 2) {
        a.Clear();
        return true;
    }
    if (a.vec_insn[0]->id == X86_INS_POP && a.vec_insn[0]->detail->x86.operands[0].reg == X86_REG_ESP && a.vec_insn[1]->id == X86_INS_RET)
    {
        _Message(emu->regs.eip, "VM exit type 1");
    }
    else if (a.vec_insn[0]->id == X86_INS_MOV && a.vec_insn[0]->detail->x86.operands[0].reg == X86_REG_ESP && a.vec_insn[1]->id == X86_INS_RET)
    {
        _Message(emu->regs.eip, "VM exit type 2");
    }
    else if (a.vec_insn[0]->id == X86_INS_MOV && a.vec_insn[0]->detail->x86.operands[0].reg == X86_REG_ESP && a.vec_insn[0]->detail->x86.operands[1].type==X86_OP_MEM)
    {
        _Message(emu->regs.eip, "VM exit type 3");
    }
    return true;
}

/**
 * \brief �ڴ���ʷ��������Ҿ߱�AntiDump��������
 * \param lpThreadParameter δʹ�ô˲���
 * \return
 */
DWORD __stdcall MemAccessAnalysis(LPVOID lpThreadParameter)
{
    emu_control = true;
    _Addtolist(0, 0, "----------------�ڴ���ʷ���[AntiDump]��ʼ----------------");
    Emulator emu;
    t_thread* thread = _Findthread(_Getcputhreadid());
    emu.MapMemoryFromOD();
    emu.SetRegFromOD();
    if (special_ins_solver_control)
    {
        emu.RegisterCallback(EmuSpecialInsSolverCallback, NULL);
    }
    emu.RegisterCallback(EmuJmpImmSolver, NULL);       //bug unicorn��bug����jmpָ����ܻ����
    emu.RegisterCallback(GetPatchSPWriteHashDataCallback, NULL);
    emu.RegisterCallback(FindSPVMExitCallback, NULL);
    emu.RegisterCallback(MemAccessAnalysisCallback, NULL);
    emu.RegisterCallback(EmuToApiCallback, _Findmemory(emu.regs.eip));
    DWORD count = emu.Run();
    _Flash("��������ֹ");
    emu.LogError();
    emu.LogEnvironment();
    _Addtolist(0, 0, "������ϣ���ģ����%lu��ָ��", count);
    _Addtolist(0, 0, "----------------�ڴ���ʷ���[AntiDump]����----------------");
    _Setcpu(0, emu.regs.eip, 0, 0, CPU_NOFOCUS);
    return 0;
}

DWORD __stdcall EmuSpecialInsSolver(LPVOID lpThreadParameter)
{
    special_ins_solver_control = 1;
    DWORD data = 0;
    if (_Getlong("cpuid_eax", &data, 4, '0', DIA_HEXONLY) == 0)
    {
        cpuid_eax = data;
    }
    if (_Getlong("cpuid_ecx", &data, 4, '0', DIA_HEXONLY) == 0)
    {
        cpuid_ecx = data;
    }
    if (_Getlong("cpuid_edx", &data, 4, '0', DIA_HEXONLY) == 0)
    {
        cpuid_edx = data;
    }
    if (_Getlong("cpuid_ebx", &data, 4, '0', DIA_HEXONLY) == 0)
    {
        cpuid_ebx = data;
    }
    return 0;
}



/*
 *\brief FixSpIATʹ�õĻص�
 */
bool UniversalTextIATFixCallback(Emulator* emu, void* user_data)
{
    char api_name[TEXTLEN]={};
    DWORD i = (DWORD)(DWORD*)user_data;
    if(_Findsymbolicname(emu->regs.eip, api_name) > 1)
    {
        emu->Stop();
    }
    else if(emu->regs.eip == i+5 || emu->regs.eip == i+6)
    {
        emu->Stop();
    }
    return true;
}

/**
 * \brief ͨ��IAT�޸����Ѳ���vmp3.8���˺����������ƣ��Ӿ���������������޷�����apphelp.dll
 * \param lpThreadParameter δʹ�ô˲���
 * \return 
 */
DWORD __stdcall UniversalTextIATFix(LPVOID lpThreadParameter)
{
    //----------------------��ʼ�����ݣ�Ѱ�Ҵ���ںͻ�����---------------------
    t_dump* item = (t_dump*)lpThreadParameter;
    HANDLE hprocess = (HANDLE)_Plugingetvalue(VAL_HPROCESS);
    t_thread* thread = _Findthread(_Getcputhreadid());
    t_memory* mem_code = _Findmemory(item->base);
    if (mem_code == nullptr)
    {
        MessageBoxA(0, "δ����0x401000������������", "����", MB_TOPMOST | MB_ICONWARNING | MB_OK);
        return 0;
    }
    t_memory* mem_vmp0 = {};
    t_table memory_table = *(t_table*)_Plugingetvalue(VAL_MEMORY);
    t_sorted memory_data = memory_table.data;
    for (int i = 0; i < memory_data.n; i++)
    {
        t_memory* memory = (t_memory*)_Getsortedbyselection(&memory_data, i);
        if (strcmp(memory->sect, ".vmp0") == 0)
        {
            mem_vmp0 = memory;
            goto next;
        }
    }
    DWORD shellcode_addr;
    //δ���Զ�������.vmp0�Σ��ֶ�ָ����������ε�ַ
    if (_Getlong("��ָ����������ε�ַ", &shellcode_addr, 4, '0', DIA_HEXONLY) != 0)
    {
        return -1;
    }
    mem_vmp0 = _Findmemory(shellcode_addr);
    if(mem_vmp0==nullptr)
    {
        MessageBoxA(0, "δ����ָ����ַ�������ڴ棬��������\n�κ�һ�����ڻ��������ڴ��ĵ�ַ����", "����", MB_TOPMOST | MB_ICONWARNING | MB_OK);
        return -1;
    }
next:
    //-------------------------��ȡ����ζ���������-------------------------------------
    SEG_MAP seg_map[2];
    //��ȡ.text��.svmp1�Ĵ���
    seg_map[0] = { mem_code->base,mem_code->size,"",new uchar[mem_code->size] };
    _Readmemory(seg_map[0].buf, mem_code->base, mem_code->size, MM_RESILENT);
    seg_map[1] = { mem_vmp0->base,mem_vmp0->size,"",new uchar[mem_vmp0->size] };
    _Readmemory(seg_map[1].buf, mem_vmp0->base, mem_vmp0->size, MM_RESILENT);
    //---------------------------��ʼ��ģ����---------------------------------------
    emu_control = true;
    Emulator emu;
    emu.MapMemoryFromOD();
    emu.SetRegFromOD();
    emu.RegisterCallback(EmuJmpImmSolver, NULL);
    //-----------------------��ʼ��ͳ���������޸�����--------------------------------
    DWORD tmp_iat_begin;
    if (_Getlong("��ʱ������ʼλ��", &tmp_iat_begin, 4, '0', DIA_HEXONLY) != 0)
    {
        return -1;
    }
    DWORD cnt_call = 0;
    DWORD cnt_jmp = 0;
    DWORD cnt_mov = 0;
    vector<api_info> vec_iat_data;	                     //��ŵȴ��ؽ���IAT����
    vector<pair<DWORD32, DWORD32>> vec_err_call;         //��һ������Ϊentry���ڶ�������Ϊʵ�ʵ���api�ĵ�ַ��δ֪�����쳣������Щ��ַ��Ҫ�ֶ�����
    char dll_path[MAX_PATH] = {};
    char api_name[TEXTLEN] = {};
    t_module* module;
    for (DWORD i = mem_code->base; i < mem_code->base + mem_code->size - 100; i++)
    {
        //����iat��������
        if (seg_map[0].buf[i - mem_code->base] == 0xE8)
        {
            a.Clear();
            a.Disasm(i, seg_map[0].buf + i - mem_code->base, 5, 1);
            DWORD jmp_to = 0;
            if (a.count != 1) continue;
            jmp_to = (DWORD)a.vec_insn[0]->detail->x86.operands[0].imm;
            if (jmp_to < mem_vmp0->base || jmp_to >= mem_vmp0->base + mem_vmp0->size) continue;
            emu.regs.eax = 0;
            emu.regs.ecx = 0;
            emu.regs.edx = 0;
            emu.regs.ebx = 0;
            emu.regs.esp = thread->reg.r[REG_ESP];
            emu.regs.ebp = 0;
            emu.regs.esi = 0;
            emu.regs.edi = 0;
            emu.regs.eip = i;
            emu.regs.efl = 0x246;
            DWORD v=0x0;
            //mov����ʱ��ֹ���Ĵ����ָ��ɲ�Ϊ0��ֵ����esp��0
            emu.WriteMemory(emu.regs.esp,4,&v);
            emu.WriteMemory(emu.regs.esp+4,4,&v);
            emu.Run(1);
            emu.RegisterCallback(UniversalTextIATFixCallback, (void*)i);
            emu.Run();
            emu.UnRegisterCallback(UniversalTextIATFixCallback);
            DWORD ret;
            emu.ReadMemory(emu.regs.esp,4,&ret);
            //��ͨ��eipɸѡmov����
            if(emu.regs.eip == i+5||emu.regs.eip==i+6)
            {
                //mov
                if (emu.regs.eax != 0 && _Findsymbolicname(emu.regs.eax, api_name) > 1)
                {
                    module = _Findmodule(emu.regs.eax);
                    strcpy_s(dll_path, module->name);
                    if(emu.regs.eip == i+5)
                    {
                        vec_iat_data.push_back({ dll_path, api_name, i-1, emu.regs.eax, 3 });
                        _Addtolist(i-1, 0, "[moye] |push call -> mov | entry = %#08x api -> %#08x %s", i-1, emu.regs.eax, api_name);
                    }
                    else if(emu.regs.eip == i+6)
                    {
                         vec_iat_data.push_back({ dll_path, api_name, i, emu.regs.eax, 3 });
                        _Addtolist(i, 0, "[moye] |call retn -> mov | entry = %#08x api -> %#08x %s", i, emu.regs.eax, api_name);
                    }
                    ++cnt_mov;
                }
                else if (emu.regs.ecx != 0 && _Findsymbolicname(emu.regs.ecx, api_name) > 1)
                {
                    module = _Findmodule(emu.regs.ecx);
                    strcpy_s(dll_path, module->name);
                    if(emu.regs.eip == i+5)
                    {
                        vec_iat_data.push_back({ dll_path, api_name, i-1, emu.regs.ecx, 3 });
                        _Addtolist(i-1, 0, "[moye] |push call -> mov | entry = %#08x api -> %#08x %s", i-1, emu.regs.ecx, api_name);
                    }
                    else if(emu.regs.eip == i+6)
                    {
                         vec_iat_data.push_back({ dll_path, api_name, i, emu.regs.ecx, 3 });
                        _Addtolist(i, 0, "[moye] |call retn -> mov | entry = %#08x api -> %#08x %s", i, emu.regs.ecx, api_name);
                    }
                    ++cnt_mov;
                }
                else if (emu.regs.edx != 0 && _Findsymbolicname(emu.regs.edx, api_name) > 1)
                {
                    module = _Findmodule(emu.regs.edx);
                    strcpy_s(dll_path, module->name);
                    if(emu.regs.eip == i+5)
                    {
                        vec_iat_data.push_back({ dll_path, api_name, i-1, emu.regs.edx, 3 });
                        _Addtolist(i-1, 0, "[moye] |push call -> mov | entry = %#08x api -> %#08x %s", i-1, emu.regs.edx, api_name);
                    }
                    else if(emu.regs.eip == i+6)
                    {
                         vec_iat_data.push_back({ dll_path, api_name, i, emu.regs.edx, 3 });
                        _Addtolist(i, 0, "[moye] |call retn -> mov | entry = %#08x api -> %#08x %s", i, emu.regs.edx, api_name);
                    }
                    ++cnt_mov;
                }
                else if (emu.regs.ebx != 0 && _Findsymbolicname(emu.regs.ebx, api_name) > 1)
                {
                    module = _Findmodule(emu.regs.ebx);
                    strcpy_s(dll_path, module->name);
                    if(emu.regs.eip == i+5)
                    {
                        vec_iat_data.push_back({ dll_path, api_name, i-1, emu.regs.ebx, 3 });
                        _Addtolist(i-1, 0, "[moye] |push call -> mov | entry = %#08x api -> %#08x %s", i-1, emu.regs.ebx, api_name);
                    }
                    else if(emu.regs.eip == i+6)
                    {
                         vec_iat_data.push_back({ dll_path, api_name, i, emu.regs.ebx, 3 });
                        _Addtolist(i, 0, "[moye] |call retn -> mov | entry = %#08x api -> %#08x %s", i, emu.regs.ebx, api_name);
                    }
                    ++cnt_mov;
                }
                else if (emu.regs.ebp != 0 && _Findsymbolicname(emu.regs.ebp, api_name) > 1)
                {
                    module = _Findmodule(emu.regs.ebp);
                    strcpy_s(dll_path, module->name);
                    if(emu.regs.eip == i+5)
                    {
                        vec_iat_data.push_back({ dll_path, api_name, i-1, emu.regs.ebp, 3 });
                        _Addtolist(i-1, 0, "[moye] |push call -> mov | entry = %#08x api -> %#08x %s", i-1, emu.regs.ebp, api_name);
                    }
                    else if(emu.regs.eip == i+6)
                    {
                         vec_iat_data.push_back({ dll_path, api_name, i, emu.regs.ebp, 3 });
                        _Addtolist(i, 0, "[moye] |call retn -> mov | entry = %#08x api -> %#08x %s", i, emu.regs.ebp, api_name);
                    }
                    ++cnt_mov;
                }
                else if (emu.regs.esi != 0 && _Findsymbolicname(emu.regs.esi, api_name) > 1)
                {
                    module = _Findmodule(emu.regs.esi);
                    strcpy_s(dll_path, module->name);
                    if(emu.regs.eip == i+5)
                    {
                        vec_iat_data.push_back({ dll_path, api_name, i-1, emu.regs.esi, 3 });
                        _Addtolist(i-1, 0, "[moye] |push call -> mov | entry = %#08x api -> %#08x %s", i-1, emu.regs.esi, api_name);
                    }
                    else if(emu.regs.eip == i+6)
                    {
                         vec_iat_data.push_back({ dll_path, api_name, i, emu.regs.esi, 3 });
                        _Addtolist(i, 0, "[moye] |call retn -> mov | entry = %#08x api -> %#08x %s", i, emu.regs.esi, api_name);
                    }
                    ++cnt_mov;
                }
                else if (emu.regs.edi != 0 && _Findsymbolicname(emu.regs.edi, api_name) > 1)
                {
                    module = _Findmodule(emu.regs.edi);
                    strcpy_s(dll_path, module->name);
                    if(emu.regs.eip == i+5)
                    {
                        vec_iat_data.push_back({ dll_path, api_name, i-1, emu.regs.edi, 3 });
                        _Addtolist(i-1, 0, "[moye] |push call -> mov | entry = %#08x api -> %#08x %s", i-1, emu.regs.edi, api_name);
                    }
                    else if(emu.regs.eip == i+6)
                    {
                         vec_iat_data.push_back({ dll_path, api_name, i, emu.regs.edi, 3 });
                        _Addtolist(i, 0, "[moye] |call retn -> mov | entry = %#08x api -> %#08x %s", i, emu.regs.edi, api_name);
                    }
                    ++cnt_mov;
                }
                else
                {
                    _Addtolist(i, 1, "[moye] δ֪�ļ��ܷ�ʽ entry = %#08x api -> %#08x", i, emu.regs.eip);
                    vec_err_call.emplace_back(i, emu.regs.eip);
                    continue;
                }
            }
            else if (ret==i+5||ret==i+6)
            {
                //call
                module = _Findmodule(emu.regs.eip);
                strcpy_s(dll_path, module->name);
                if(_Findsymbolicname(emu.regs.eip, api_name) > 1)
                {
                    if(ret==i+5)
                    {
                        vec_iat_data.push_back({ dll_path, api_name, i-1, emu.regs.eip, 2 });
                        _Addtolist(i-1, 0, "[moye] |push call -> call| entry = %#08x api -> %#08x %s", i-1, emu.regs.eip, api_name);
                    }
                    else
                    {
                        vec_iat_data.push_back({ dll_path, api_name, i, emu.regs.eip, 2 });
                        _Addtolist(i, 0, "[moye] |call retn -> call| entry = %#08x api -> %#08x %s", i, emu.regs.eip, api_name);
                    }
                    ++cnt_call;
                }
                else
                {
                    _Addtolist(i, 1, "[moye] δ֪�ļ��ܷ�ʽ entry = %#08x api -> %#08x", i, emu.regs.eip);
                    vec_err_call.emplace_back(i, emu.regs.eip);
                    continue;
                }
            }
            else
            {
                DWORD v = 0;
                emu.ReadMemory(emu.regs.esp,4,&v);
                if(v==0)
                {
                    //jmp
                    module = _Findmodule(emu.regs.eip);
                    if(module == NULL)
                    {
                        _Addtolist(i, 1, "[moye] δ֪�ļ��ܷ�ʽ entry = %#08x api -> %#08x", i, emu.regs.eip);
                        vec_err_call.emplace_back(i, emu.regs.eip);
                        continue;
                    }
                    strcpy_s(dll_path, module->name);
                    if (_Findsymbolicname(emu.regs.eip, api_name) > 1)
                    {
                        if (emu.regs.esp == thread->reg.r[REG_ESP]+8)
                        {
                            vec_iat_data.push_back({ dll_path, api_name, i - 1, emu.regs.eip, 1 });
                            _Addtolist(i-1, 0, "[moye] |push call -> jmp | entry = %#08x api -> %#08x %s", i - 1, emu.regs.eip, api_name);
                        }
                        else if (emu.regs.esp == thread->reg.r[REG_ESP]+4)
                        {
                            vec_iat_data.push_back({ dll_path, api_name, i, emu.regs.eip, 1 });
                            _Addtolist(i, 0, "[moye] |call retn -> jmp | entry = %#08x api -> %#08x %s", i, emu.regs.eip, api_name);
                        }
                        else
                        {
                            _Addtolist(i, 1, "[moye] δ֪�ļ��ܷ�ʽ entry = %#08x api -> %#08x", i, emu.regs.eip);
                            vec_err_call.emplace_back(i, emu.regs.eip);
                            continue;
                        }
                        ++cnt_jmp;
                    }
                    else
                    {
                        _Addtolist(i, 1, "[moye] δ֪�ļ��ܷ�ʽ entry = %#08x api -> %#08x", i, emu.regs.eip);
                        vec_err_call.emplace_back(i, emu.regs.eip);
                        continue;
                    }
                }
                else
                {
                    _Addtolist(i, 1, "[moye] δ֪�ļ��ܷ�ʽ entry = %#08x api -> %#08x", i, emu.regs.eip);
                    vec_err_call.emplace_back(i, emu.regs.eip);
                    continue;
                }
            }
            _Infoline("[moye] ������ɣ�entry -> %#08X api -> %-20s", i, api_name);
            i+=4;
        }
    }

    delete[] seg_map[0].buf;
    delete[] seg_map[1].buf;
    //-----------------------�ؽ�iat��------------------------
    if (vec_iat_data.empty())
    {
        MessageBoxA(0, "δ���ҵ���Ҫ�޸��ĵط�", "��ʾ", MB_TOPMOST | MB_ICONWARNING | MB_OK);
        return 0;
    }
    sort(vec_iat_data.begin(), vec_iat_data.end());     //����

    DWORD tmp_addr = tmp_iat_begin;    //��ǰapi��ŵĵ�ַ
    char tmp[50] = {};                 //����ַ���������
    char errtext[TEXTLEN];
    t_asmmodel asmmodel;
    vec_iat_data.push_back({});   //ĩβ��ǣ����ҷ�Խ��
    for (DWORD i = 0; i < vec_iat_data.size() - 1; i++)
    {
        _Progress(i * 1000 / (vec_iat_data.size() - 1), (char*)"�ؽ�IAT����...����");
        switch (vec_iat_data[i].type)
        {
            case 1:
                //jmp
                _Writememory(&vec_iat_data[i].api_addr, tmp_addr, 4, MM_SILENT);
                _Writememory((BYTE*)"\xFF\x25", vec_iat_data[i].fix_addr, 2, MM_SILENT);
                _Writememory(&tmp_addr, vec_iat_data[i].fix_addr + 2, 4, MM_SILENT);
                break;
            case 2:
                //call
                _Writememory(&vec_iat_data[i].api_addr, tmp_addr, 4, MM_SILENT);
                _Writememory((BYTE*)"\xFF\x15", vec_iat_data[i].fix_addr, 2, MM_SILENT);
                _Writememory(&tmp_addr, vec_iat_data[i].fix_addr + 2, 4, MM_SILENT);

                break;
            case 3:
                _Writememory(&vec_iat_data[i].api_addr, tmp_addr, 4, MM_SILENT);
                sprintf_s(tmp, "mov eax, dword ptr [%08X]", tmp_addr);
                _Assemble(tmp, vec_iat_data[i].fix_addr, &asmmodel, 0, 0, errtext);
                _Writememory(asmmodel.code, vec_iat_data[i].fix_addr, asmmodel.length, MM_SILENT);
                break;
            case 4:
                _Writememory(&vec_iat_data[i].api_addr, tmp_addr, 4, MM_SILENT);
                sprintf_s(tmp, "mov ecx, dword ptr [%08X]", tmp_addr);
                _Assemble(tmp, vec_iat_data[i].fix_addr, &asmmodel, 0, 0, errtext);
                _Writememory(asmmodel.code, vec_iat_data[i].fix_addr, asmmodel.length, MM_SILENT);
                break;
            case 5:
                _Writememory(&vec_iat_data[i].api_addr, tmp_addr, 4, MM_SILENT);
                sprintf_s(tmp, "mov edx, dword ptr [%08X]", tmp_addr);
                _Assemble(tmp, vec_iat_data[i].fix_addr, &asmmodel, 0, 0, errtext);
                _Writememory(asmmodel.code, vec_iat_data[i].fix_addr, asmmodel.length, MM_SILENT);
                break;
            case 6:
                _Writememory(&vec_iat_data[i].api_addr, tmp_addr, 4, MM_SILENT);
                sprintf_s(tmp, "mov ebx, dword ptr [%08X]", tmp_addr);
                _Assemble(tmp, vec_iat_data[i].fix_addr, &asmmodel, 0, 0, errtext);
                _Writememory(asmmodel.code, vec_iat_data[i].fix_addr, asmmodel.length, MM_SILENT);
                break;
            case 7:
                _Writememory(&vec_iat_data[i].api_addr, tmp_addr, 4, MM_SILENT);
                sprintf_s(tmp, "mov ebp, dword ptr [%08X]", tmp_addr);
                _Assemble(tmp, vec_iat_data[i].fix_addr, &asmmodel, 0, 0, errtext);
                _Writememory(asmmodel.code, vec_iat_data[i].fix_addr, asmmodel.length, MM_SILENT);
                break;
            case 8:
                _Writememory(&vec_iat_data[i].api_addr, tmp_addr, 4, MM_SILENT);
                sprintf_s(tmp, "mov esi, dword ptr [%08X]", tmp_addr);
                _Assemble(tmp, vec_iat_data[i].fix_addr, &asmmodel, 0, 0, errtext);
                _Writememory(asmmodel.code, vec_iat_data[i].fix_addr, asmmodel.length, MM_SILENT);
                break;
            case 9:
                _Writememory(&vec_iat_data[i].api_addr, tmp_addr, 4, MM_SILENT);
                sprintf_s(tmp, "mov edi, dword ptr [%08X]", tmp_addr);
                _Assemble(tmp, vec_iat_data[i].fix_addr, &asmmodel, 0, 0, errtext);
                _Writememory(asmmodel.code, vec_iat_data[i].fix_addr, asmmodel.length, MM_SILENT);
                break;
            default:
                MessageBoxA(0, "�ؽ�IAT��ʱ�쳣������ȷ������", "����", MB_TOPMOST | MB_ICONERROR | MB_OK);
                break;
        }

        if (vec_iat_data[i].dll_name != vec_iat_data[i + 1].dll_name)
        {
            //��ͬdll�ĺ������ϣ���0����
            DWORD data = 0;
            tmp_addr += 4;
            _Writememory(&data, tmp_addr, 4, MM_SILENT);
            tmp_addr += 4;
        }
        else if (vec_iat_data[i].api_name != vec_iat_data[i + 1].api_name)
        {
            //���������ͬ�ĺ������������������µĺ�������Ҫ����4�ֽڷ����ĵ�ַ
            tmp_addr += 4;
        }
    }
    _Progress(0, 0);
    _Addtolist(0, 0, "[moye] --------------�޸����--------------");
    _Addtolist(0, 0, "[moye] �޸�Ϊcall��������%lu", cnt_call);
    _Addtolist(0, 0, "[moye] �޸�Ϊjmp��������%lu", cnt_jmp);
    _Addtolist(0, 0, "[moye] �޸�Ϊmov��������%lu", cnt_mov);
    _Addtolist(0, 0, "[moye] ��ʱ���ݵ�ַ��0x%08x", tmp_iat_begin);
    _Addtolist(0, 0, "[moye] ��ʱ���ݴ�С��0x%08x", tmp_addr - tmp_iat_begin);
    _Addtolist(0, 0, "[moye] ------------------------------------");
    if (!vec_err_call.empty())
    {
        _Addtolist(0, 1, "[moye] --------����������Ҫ�ֶ�����--------");
        for (auto& i : vec_err_call)
        {
            _Addtolist(i.first, 1, "[moye] entry->0x%08x api->0x%08x", i.first, i.second);
        }
        _Addtolist(0, 1, "[moye] ------------------------------------");
    }
    _Flash("�޸����");
    return 0;
}


//
//
// void SpTestPatch()
// {
//     t_thread* thread = _Findthread(_Getcputhreadid());
//     fs_base=thread->reg.base[SEG_FS];
//     _Writememory(PEHeader,0x00400000,0x400,MM_SILENT);
//     WORD pid=0x57b8;
//     _Writememory(&pid,fs_base+0x20,0x2,MM_SILENT);
//     _Writememory(&pid,fs_base+0x6b4,0x2,MM_SILENT);
//     DWORD peb_addr=0;
//     _Readmemory(&peb_addr,fs_base+0x30,4,MM_SILENT);
//     DWORD ldr_addr=0;
//     _Readmemory(&ldr_addr,peb_addr+0xc,4,MM_SILENT);
//     DWORD InMemoryOrderModuleList_addr;
//     _Readmemory(&InMemoryOrderModuleList_addr,ldr_addr+0x14,4,MM_SILENT);
//     DWORD dll_base=0;
//     _Readmemory(&dll_base,InMemoryOrderModuleList_addr+0x10,4,MM_SILENT);
//     WORD len=0x48;
//     _Writememory(&len,InMemoryOrderModuleList_addr+0x1c,2,MM_SILENT);
//     DWORD path_addr=0;
//     _Readmemory(&path_addr,InMemoryOrderModuleList_addr+0x20,4,MM_SILENT);
//     _Writememory(ProcessFullPath,path_addr,sizeof(ProcessFullPath),MM_SILENT);
//
//     
// }
//
//

/**
 * \brief �����ڴ洰���е��ַ��������а�
 * \param dump
 */
void GetString(t_dump* dump)
{
    if (dump == NULL)
    {
        return;
    }
    const DWORD size = dump->sel1 - dump->sel0;
    char* str = new char[size + 1];
    memset(str, 0, size + 1);
    _Readmemory(str, dump->sel0, size, MM_SILENT);
    if (OpenClipboard(0))
    {
        EmptyClipboard();
        HGLOBAL hClip;
        hClip = GlobalAlloc(GMEM_MOVEABLE, size + 1);
        char* pBuf;
        pBuf = (char*)GlobalLock(hClip);
        strcpy_s(pBuf, size + 1, str);
        GlobalUnlock(hClip);
        SetClipboardData(CF_TEXT, hClip);
        CloseClipboard();
        _Message(0, "[moye] �������  ���ȣ�%lu�ֽ�", size);
    }
    else
    {
        _Addtolist(0, 1, "[moye] �򿪼��а�ʧ��");
        _Flash("�򿪼��а�ʧ��");
    }
    delete[] str;
}

void GetBinArray(t_dump* dump)
{
    if (dump == NULL)
    {
        return;
    }
    const DWORD len = dump->sel1 - dump->sel0;
    BYTE* str = new BYTE[len + 1];
    memset(str, 0, len + 1);
    _Readmemory(str, dump->sel0, len, MM_SILENT);
    std::string str_arr = "BYTE array[] = {";
    for (DWORD i = 0; i < len; i++)
    {
        if (i % 20 == 0) str_arr += "\n\t";
        char temp[8];
        sprintf_s(temp, "0x%02X,", str[i]);
        str_arr += temp;
    }
    str_arr[str_arr.size() - 1] = '}';
    str_arr += ";\n";
    const DWORD str_size = str_arr.size();
    if (OpenClipboard(0))
    {
        EmptyClipboard();
        HGLOBAL hClip;
        hClip = GlobalAlloc(GMEM_MOVEABLE, str_size + 1);
        char* pBuf;
        pBuf = (char*)GlobalLock(hClip);
        strcpy_s(pBuf, str_size + 1, str_arr.c_str());
        GlobalUnlock(hClip);
        SetClipboardData(CF_TEXT, hClip);
        CloseClipboard();
        _Message(0, "[moye] �������  ���鳤�ȣ�%lu�ֽ�  ���ݳ��ȣ�%lu�ֽ�", len, str_size);
    }
    else
    {
        _Addtolist(0, 1, "[moye] �򿪼��а�ʧ��");
        _Flash("�򿪼��а�ʧ��");
    }
    delete[] str;
}


/**
 * \brief ע����쳣������
 */
LONG __stdcall ExceptionHandler(EXCEPTION_POINTERS* ExceptionInfo)
{
    char msg[256];
    sprintf_s(msg, "[moye] OD�����쳣��������룺%08X", ExceptionInfo->ExceptionRecord->ExceptionCode);
    MessageBoxA(0, msg, "����", MB_TOPMOST | MB_ICONERROR | MB_OK);
    return EXCEPTION_CONTINUE_SEARCH;
}


/**
 * \brief ����ĵ�������
 * \param shortname �˵�����ʾ�Ĳ����
 * \return
 */
extern "C" __declspec(dllexport) int ODBG_Plugindata(char* shortname)
{
    // h_exp_handler = AddVectoredExceptionHandler(0, ExceptionHandler);
    // if (h_exp_handler == NULL)
    // {
    //     _Addtolist(0, 1, "ע���쳣������ʧ��");
    // }
    const char* pluginName = "įҶ��OD���v8.0";
    strcpy_s(shortname, strlen(pluginName) + 1, pluginName);
    return PLUGIN_VERSION;
}

/**
 * \brief ����ĵ������� �����ʼ���������жϺͲ����֧�ֵİ汾�Ƿ�һ��
 * \param ollydbgversion    ��ǰOD�汾��
 * \param hw                OllyDbg �����ھ��
 * \param features          ����
 * \return
 */
extern "C" __declspec(dllexport) int ODBG_Plugininit(int ollydbgversion, HWND hw, ulong * features)
{
    char msg[200] = {};
    sprintf_s(msg, "  ����ʱ�䣺%s %s", __DATE__, __TIME__);
    _Addtolist(0, 0, "įҶ��OD���v8.0");
    _Addtolist(0, -1, msg);
    if (ollydbgversion < PLUGIN_VERSION)
    {
        MessageBoxA(hw, "�������֧�ֵ�ǰ�汾OD!", "įҶ��OD���v8.0", MB_TOPMOST | MB_ICONERROR | MB_OK);
        return -1;
    }
    //g_hOllyDbg = hw;
    return 0;
}


/**
 * \brief ��Ҫ�ĵ�����������ʾ�˵���
 * \param origin    ���� ODBG_Pluginmenu �����Ĵ��ڴ���
 * \param data      ָ��4K�ֽڳ��Ļ����������ڽ��������˵��Ľṹ
 * \param item      ָ����ѡ���Ԫ�أ�����������ʾ�ڵ�ǰ���ڵģ�Ҳ������ת�������еģ�ָ��ת������������ΪNULL
 * \return
 */
extern "C" __declspec(dllexport) cdecl int  ODBG_Pluginmenu(int origin, TCHAR data[4096], VOID * item)
{
    if (origin == PM_MAIN)
    {
        strcpy_s(data, 4096, "0&����");
    }
    if (origin == PM_DISASM)
    {
        strcpy_s(data, 4096, "įҶ��OD���{0&��ǩ,1&�����ڴ�,2&�ϲ�����ʽdump,3&������api,4&ģ����api,5&ͨ��IAT�޸�[����ת���������ʹ��],6&�޸�sp�����(����ƥ��),7&�ڴ���ʷ���[AntiDump],8&�ж�ģ����,9&ģ��ʱhook cpuid}");
    }
    if (origin == PM_CPUDUMP)
    {
        strcpy_s(data, 4096, "įҶ��OD���{0&�����ַ���,1&����Ϊ����������}");
    }
    return 1;
}

/**
 * \brief �˵�����ִ�д˺��������еĲ˵���������ִ�е��������
 * \param origin    ������� ODBG_Pluginaction �����Ĵ��ڵĴ���
 * \param action    �˵����Ӧ����(0..63)����ODBG_Pluginmenu ������
 * \param item      ָ����ѡ������ݣ�����������ʾ�ڵ�ǰ���ڵģ�����ת�������еģ�ָ�� dump�ṹ����Ϊ NULL
 */
extern "C" __declspec(dllexport) cdecl void ODBG_Pluginaction(int origin, int action, VOID * item)
{
    //�������ڵ��
    if (origin == PM_MAIN)
    {
        if (action == 0)
        {
            char msg[256];
            sprintf_s(msg, "cpu���ڡ����ݴ��� �Ҽ��ɵ��ù���\n��ע�⣩����ڹرճ���ǰֹͣģ����\n����֤�������Լ�ʹ�õ�ģ�鲻����bug�����ܻ���ڱ�������\n��������ѧϰʹ��\n����ʱ�䣺%s %s", __DATE__, __TIME__);
            MessageBoxA(g_hOllyDbg, msg, "����", MB_TOPMOST | MB_ICONINFORMATION | MB_OK);
        }
    }
    //�ڷ���ര�ڵ��
    if (origin == PM_DISASM)
    {
        if (action == 0)
        {
            CreateThread(0, 0, RenameCall, item, 0, 0);
        }
        else if (action == 1)
        {
            CreateThread(0, 0, AllocMemory, 0, 0, 0);
        }
        else if (action == 2)
        {
            CreateThread(0, 0, MergeDump, 0, 0, 0);
        }
        else if (action == 3)
        {
            TraceToApi();
        }
        else if (action == 4)
        {
            CreateThread(0, 0, EmuToApi, 0, 0, 0);
        }
        else if (action == 5)
        {
            CreateThread(0, 0, UniversalTextIATFix, (t_dump*)item, 0, 0);
        }
        else if (action == 6)
        {
            CreateThread(0, 0, FixSpIAT, 0, 0, 0);
        }
        else if (action == 7)
        {
            CreateThread(0, 0, MemAccessAnalysis, 0, 0, 0);
        }
        else if (action == 8)
        {
            emu_control = false;
        }
        else if (action == 9)
        {
            CreateThread(0, 0, EmuSpecialInsSolver, 0, 0, 0);
        }
    }
    //�����ݴ��ڵ��
    if (origin == PM_CPUDUMP)
    {
        if (action == 0)
        {
            GetString((t_dump*)item);
        }
        if (action == 1)
        {
            GetBinArray((t_dump*)item);
        }
    }
}

extern "C" __declspec(dllexport) cdecl void ODBG_Pluginreset()
{
    emu_control = false;
}

extern "C" __declspec(dllexport) cdecl void ODBG_Plugindestroy()
{
    emu_control = false;
}
