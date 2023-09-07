#pragma once
#include <vector>
#include <Windows.h>
#include "capstone-5.0/include/capstone/capstone.h"

class Analyzer
{
public:
    csh csh;							    //capstone������û���Ӧ���޸�����
    cs_err cs_error;			            //capstone�����루�û���Ӧ���޸�����
    std::vector<cs_insn*> vec_insn;		    //capstone������ָ����û�Ӧ��ͨ����Ա�����޸�����
    DWORD count;						    //ָ�������û���Ӧ��ֱ���޸�����

    Analyzer();

    ~Analyzer();

    /**
     * \brief �����ָ������
     * \param base [in]��һ��ָ���VirtualAddress
     * \param code [in]�����ƴ��뻺����
     * \param size [in]��������С
     * \param max_count [in]��󷴻��ָ����, Ϊ0������
     */
    void Disasm(const DWORD& base, BYTE* code, const DWORD& code_size, const DWORD& max_count = 1);

    /**
     * \brief ��շ������
     */
    void Clear();
};
