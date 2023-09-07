#pragma once
#include <string>
#include <vector>
#include "ollydbg-sdk/Plugin.h"
#include "unicorn-2.0.1-win32/include/unicorn/unicorn.h"
#include "capstone-5.0/include/capstone/capstone.h"


struct SEG_MAP {
    DWORD				base;
    unsigned int		size;
    std::string	file_name;
    BYTE* buf;
    SEG_MAP()
    {
        base = 0;
        size = 0;
        file_name = "";
        buf = nullptr;
    }
    SEG_MAP(DWORD base, unsigned int size, const std::string& file_name, BYTE* buf)
    {
        this->base = base;
        this->size = size;
        this->file_name = file_name;
        this->buf = buf;
    }
    SEG_MAP& operator=(const SEG_MAP& seg)
    {
        base = seg.base;
        size = seg.size;
        file_name = seg.file_name;
        buf = seg.buf;
        return *this;
    }
};

class Emulator
{
    struct REGS {
        union
        {
            DWORD eax;
            struct
            {
                union
                {
                    WORD ax;
                    struct
                    {
                        BYTE al;
                        BYTE ah;
                    };
                };
                WORD eax_h;
            };
        };
        union
        {
            DWORD ecx;
            struct
            {
                union
                {
                    WORD cx;
                    struct
                    {
                        BYTE cl;
                        BYTE ch;
                    };
                };
                WORD ecx_h;
            };
        };
        union
        {
            DWORD edx;
            struct
            {
                union
                {
                    WORD dx;
                    struct
                    {
                        BYTE dl;
                        BYTE dh;
                    };
                };
                WORD edx_h;
            };
        };
        union
        {
            DWORD ebx;
            struct
            {
                union
                {
                    WORD bx;
                    struct
                    {
                        BYTE bl;
                        BYTE bh;
                    };
                };
                WORD ebx_h;
            };
        };
        union
        {
            DWORD esp;
            struct
            {
                union
                {
                    WORD sp;
                    struct
                    {
                        BYTE sp_l;
                        BYTE sp_h;
                    };
                };
                WORD esp_h;
            };
        };
        union
        {
            DWORD ebp;
            struct
            {
                union
                {
                    WORD bp;
                    struct
                    {
                        BYTE bp_l;
                        BYTE bp_h;
                    };
                };
                WORD ebp_h;
            };
        };
        union
        {
            DWORD esi;
            struct
            {
                union
                {
                    WORD si;
                    struct
                    {
                        BYTE si_l;
                        BYTE si_h;
                    };
                };
                WORD esi_h;
            };
        };
        union
        {
            DWORD edi;
            struct
            {
                union
                {
                    WORD di;
                    struct
                    {
                        BYTE di_l;
                        BYTE di_h;
                    };
                };
                WORD edi_h;
            };
        };
        DWORD eip;
        union
        {
            DWORD efl;
            struct {
                unsigned CF : 1;
                unsigned reserved1 : 1;
                unsigned PF : 1;
                unsigned reserved2 : 1;
                unsigned AF : 1;
                unsigned reserved3 : 1;
                unsigned ZF : 1;
                unsigned SF : 1;
                unsigned TF : 1;
                unsigned IF : 1;
                unsigned DF : 1;
                unsigned OF : 1;
                unsigned IOPL : 2;
                unsigned NT : 1;
                unsigned reserved4 : 1;
                unsigned RF : 1;
                unsigned VM : 1;
                unsigned AC : 1;
                unsigned VIF : 1;
                unsigned VIP : 1;
                unsigned ID : 1;
                unsigned reserved5 : 10;
            };
        };
        DWORD fs_base;
    };
    
    /**
     * \brief �ص��������壬��һ������Ϊָ�������thisָ�룬user_dataΪע��ص�ʱ���ݵĲ���
     * \return ������ʱ��������ִ�к���Ļص������������ڱ���ָ�ʣ�µĻص�����������ִ��
     */
    typedef bool(*EMULATOR_CALLBACK)(Emulator* emu, void* user_data);

    uc_engine* uc;	                                                //unicorn���
    uc_err uc_error;												//unicorn������Ϣ
    
    std::vector<SEG_MAP> seg_map;									//�ڴ沼����Ϣ 
    std::vector<std::pair<EMULATOR_CALLBACK, void*>> callbacks;		//��Żص�����
    DWORD run_cnt;													//ģ����ִ����ɵ�ָ����
    bool run_state;													//ģ����״̬��Ϊ1��������У�Ϊ0��ֹͣ

    /**
     * \brief �ó�Ա����regs����unicorn�ļĴ�����ֵ
     */
    void WriteUCRegs();

    /**
     * \brief ��unicorn��ȡ�Ĵ�����ֵ����ŵ���Ա����regs
     */
    void ReadUCRegs();

    //-----------------------------����ΪPublic�ӿ�-------------------------------------
public:
    REGS regs;
    uc_context* uc_ctx;												//unicorn������
    Emulator();

    ~Emulator();

    Emulator(const Emulator& emu);

    /**
     * \brief dumpģ���������ڴ�ͼĴ���״̬��dump�����ݽ��������ڵ�ǰ����Ŀ¼�µ��Զ��������ļ���
     * �����д����OD������ͻᱣ����ODĿ¼�µ�path�ļ��У�           \n
     * ʹ��ʾ����emu.Dump("save"); emu.Dump("folder1/folder2/save");
     * \param path [in] ����dump�ļ����ļ�������(�����·��)
     */
    void Dump(const std::string& path);

    /**
     * \brief ���ļ��ж�ȡ���ݣ�ӳ�䵽ģ�����ڴ���
     * \param base		[in]�ڴ�����ʼ��ַ(Virtual Address)
     * \param size		[in]�ļ��Ĵ�С������Ϊ0x1000�ı���
     * \param file_name [in]�ļ���
     * \return			���ӳ��ɹ��������档���򷵻ؼ�
     */
    bool MapFromFile(const DWORD& base, const DWORD& size, const char* file_name);

    /**
     * \brief �ӻ������ж�ȡ���ݣ�ӳ�䵽ģ�����ڴ���
     * \param base   [in]�ڴ�����ʼ��ַ(Virtual Address)
     * \param size   [in]�ڴ���С������С�ڻ������Ĵ�С��������Ϊ0x1000�ı���
     * \param buf    [in]������ָ��
     * \return       ���ӳ��ɹ��������档���򷵻ؼ�
     */
    bool MapFromMemory(const DWORD& base, const DWORD& size, void* buf);

    /**
     * \brief ע��һ���ص�������ģ����ִ��ÿ��ָ��֮ǰ�����ᰴע��ʱ��˳�����
     * \param function	[in]�Զ���Ļص�����
     * \param user_data [in]���ص��������ݵĶ��������
     * \warning ������ͬ������Ϊͬһ���ص�����,���۶�������Ƿ���ͬ���ظ�ע��ص������������κ�Ч��
     */
    void RegisterCallback(EMULATOR_CALLBACK func, void* user_data);

    /**
     * \brief ɾ��һ���ص�����
     * \param func [in]�Զ���Ļص�����
     * \warning ����ص������ڣ��������κ�Ч��
     */
    void UnRegisterCallback(EMULATOR_CALLBACK func);

    /**
     * \brief ��ʼģ��ִ��
     * \param max [in]���ִ�е�ָ���������Բ��Ĭ�ϲ�����
     * \return ִ�е�ָ����
     * \warning ����ڻص����ֶ�������ĳЩָ������޸�eipʵ��ģ��hook���ᵼ�·��ص�countֵС��������ֵ
     */
    DWORD Run(const DWORD& max = 0xFFFFFFFF);

    /**
     * \brief ֹͣģ��ִ��
     */
    void Stop();

    /**
     * \brief ���ģ�����ļĴ����Ͷ�ջ���
     */
    void PrintEnvironment();

    /**
     * \brief ���ģ����������Ϣ
     */
    void PrintError();

    /**
     * \brief ��ģ�����ж�ȡһƬ�ڴ�
     * \param addr  [in]�ڴ�������ַ
     * \param size  [in]Ҫ��ȡ�Ĵ�С
     * \param buf   [out]������ݵĻ�����
     * \return ����ɹ������棬���򷵻ؼ�
     */
    bool ReadMemory(const DWORD& addr, const DWORD& size, void* buf);

    /**
     * \brief �޸�ģ�����е�һƬ�ڴ�
     * \param addr [in]�ڴ�������ַ
     * \param size [in]Ҫд��Ĵ�С
     * \param buf  [in]������ݵĻ�����
     * \return ����ɹ������棬���򷵻ؼ�
     */
    bool WriteMemory(const DWORD& addr, const DWORD& size, const void* buf);

    /* \brief ͨ��capstone reg���ͻ�ȡģ�����мĴ�����ֵ
     * \param reg [in]capstone reg
     * \return �Ĵ�����ֵ
     */
    DWORD GetReg(const x86_reg& reg);

    /* \brief ͨ��capstone reg��������ģ�����мĴ�����ֵ
     * \param reg [in]capstone reg
     * \param value [in]��ֵ
     */
    void SetReg(const x86_reg& reg, const DWORD& value);

    /* \brief ͨ��capstone mem���ͻ�ȡģ�������ڴ�ĵ�ַ
     * \param reg [in]capstone mem
     * \return �ڴ�������ַ
     */
    DWORD GetMemAddr(const x86_op_mem& mem);

    /**
     * \brief ����ģ������ǰִ�й���ָ����
     * \return run_cnt
     */
    DWORD GetRunCount();

    /**
     * \brief ��od��־�������ģ�����ļĴ����Ͷ�ջ���
     */
    void LogEnvironment();

    /**
     * \brief ��od��־�������ģ�����ļĴ����Ͷ�ջ���
     */
    void LogError();

    /**
     * \brief ӳ��OD�����ڴ浽ģ����
     */
    void MapMemoryFromOD();

    /**
     * \brief ��OD�Ĵ�������ģ�����ļĴ���
     */
    void SetRegFromOD();

    /**
     * \brief ����ģ���������ģ����ڿ��ٻ�ԭģ��������
     */
    void SaveContext();

    /**
     * \brief ���ٻ�ԭģ��������
     */
    void RestoreContext();
};

struct SegmentDescriptor {
    union {
        struct {
            unsigned short limit0;
            unsigned short base0;
            unsigned char base1;
            unsigned char type : 4;
            unsigned char system : 1; /* S flag */
            unsigned char dpl : 2;
            unsigned char present : 1; /* P flag */
            unsigned char limit1 : 4;
            unsigned char avail : 1;
            unsigned char is_64_code : 1;  /* L flag */
            unsigned char db : 1;          /* DB flag */
            unsigned char granularity : 1; /* G flag */
            unsigned char base2;
        };
        uint64_t desc;
    };
};


#define SEGBASE(d)                                                             \
    ((uint32_t)((((d).desc >> 16) & 0xffffff) |                                \
                (((d).desc >> 32) & 0xff000000)))
#define SEGLIMIT(d) ((d).limit0 | (((unsigned int)(d).limit1) << 16))


class SegmentSelector {
public:
    union
    {
        DWORD val = 0;
        struct
        {
            unsigned rpl : 2;
            unsigned ti : 1;
            unsigned index : 13;
        };
    };
    SegmentSelector& operator =(const ulong& v)
    {
        val = v;
    }
    SegmentSelector(const ulong& v)
    {
        val = v;
    }
};
