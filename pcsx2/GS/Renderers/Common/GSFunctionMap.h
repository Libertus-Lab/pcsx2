/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2021 PCSX2 Dev Team
 *
 *  PCSX2 is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU Lesser General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  PCSX2 is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with PCSX2.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "GS/GS.h"
#include "GS/GSCodeBuffer.h"

#include "GS/Renderers/SW/GSScanlineEnvironment.h"
#include "common/General.h"

#include <xbyak/xbyak_util.h>

template <class KEY, class VALUE>
class GSFunctionMap
{
protected:
	struct ActivePtr
	{
		uint64 frame, frames, prims;
		uint64 ticks, actual, total;
		VALUE f;
	};

	std::unordered_map<KEY, ActivePtr*> m_map_active;

	ActivePtr* m_active;

	virtual VALUE GetDefaultFunction(KEY key) = 0;

public:
	GSFunctionMap()
		: m_active(NULL)
	{
	}

	virtual ~GSFunctionMap()
	{
		for (auto& i : m_map_active)
			delete i.second;
	}

	VALUE operator[](KEY key)
	{
		m_active = NULL;

		auto it = m_map_active.find(key);

		if (it != m_map_active.end())
		{
			m_active = it->second;
		}
		else
		{
			ActivePtr* p = new ActivePtr();

			memset(p, 0, sizeof(*p));

			p->frame = (uint64)-1;

			p->f = GetDefaultFunction(key);

			m_map_active[key] = p;

			m_active = p;
		}

		return m_active->f;
	}

	void UpdateStats(uint64 frame, uint64 ticks, int actual, int total, int prims)
	{
		if (m_active)
		{
			if (m_active->frame != frame)
			{
				m_active->frame = frame;
				m_active->frames++;
			}

			m_active->prims += prims;
			m_active->ticks += ticks;
			m_active->actual += actual;
			m_active->total += total;

			ASSERT(m_active->total >= m_active->actual);
		}
	}

	virtual void PrintStats()
	{
		uint64 ttpf = 0;

		for (const auto& i : m_map_active)
		{
			ActivePtr* p = i.second;

			if (p->frames)
			{
				ttpf += p->ticks / p->frames;
			}
		}

		uint64_t million = 1000 * 1000;
		uint64_t billion = 1000 * million;
		uint64_t tps = GetTickFrequency();

		printf("GS stats\n");

		printf("      key      | frames | prims |       runtime      |          pixels\n");
		printf("               |        |  #/f  |   pct   µs/f ns/px |    #/f   #/prim overdraw\n");

		for (const auto& i : m_map_active)
		{
			KEY key = i.first;
			ActivePtr* p = i.second;

			if (p->frames && p->actual && ttpf)
			{
				uint64 tpp = p->ticks  / p->actual;
				uint64 tpf = p->ticks  / p->frames;
				uint64 ppf = p->actual / p->frames;

				printf("%014llx | %6llu | %5llu | %5.2f%% %6llu %4llu | %8llu %6llu %5.2f%%\n",
					(uint64)key,
					p->frames,
					p->prims / p->frames,
					(float)(tpf * 10000 / ttpf) / 100,
					(tpf * million) / tps,
					(tpp * billion) / tps,
					ppf,
					p->actual / p->prims,
					(float)((p->total - p->actual) * 10000 / p->total) / 100);
			}
		}
	}
};

class GSCodeGenerator : public Xbyak::CodeGenerator
{
protected:
	Xbyak::util::Cpu m_cpu;

public:
	GSCodeGenerator(void* code, size_t maxsize)
		: Xbyak::CodeGenerator(maxsize, code)
	{
	}
};

template <class CG, class KEY, class VALUE>
class GSCodeGeneratorFunctionMap : public GSFunctionMap<KEY, VALUE>
{
	std::string m_name;
	void* m_param;
	std::unordered_map<uint64, VALUE> m_cgmap;
	GSCodeBuffer m_cb;
	size_t m_total_code_size;

	enum { MAX_SIZE = 8192 };

public:
	GSCodeGeneratorFunctionMap(const char* name, void* param)
		: m_name(name)
		, m_param(param)
		, m_total_code_size(0)
	{
	}

	~GSCodeGeneratorFunctionMap()
	{
#ifdef _DEBUG
		fprintf(stderr, "%s generated %zu bytes of instruction\n", m_name.c_str(), m_total_code_size);
#endif
	}

	VALUE GetDefaultFunction(KEY key)
	{
		VALUE ret = NULL;

		auto i = m_cgmap.find(key);

		if (i != m_cgmap.end())
		{
			ret = i->second;
		}
		else
		{
			void* code_ptr = m_cb.GetBuffer(MAX_SIZE);

			CG* cg = new CG(m_param, key, code_ptr, MAX_SIZE);
			ASSERT(cg->getSize() < MAX_SIZE);

#if 0
			fprintf(stderr, "%s Location:%p Size:%zu Key:%llx\n", m_name.c_str(), code_ptr, cg->getSize(), (uint64)key);
			GSScanlineSelector sel(key);
			sel.Print();
#endif

			m_total_code_size += cg->getSize();

			m_cb.ReleaseBuffer(cg->getSize());

			ret = (VALUE)cg->getCode();

			m_cgmap[key] = ret;

#ifdef ENABLE_VTUNE

			// vtune method registration

			// if(iJIT_IsProfilingActive()) // always > 0
			{
				std::string name = format("%s<%016llx>()", m_name.c_str(), (uint64)key);

				iJIT_Method_Load ml;

				memset(&ml, 0, sizeof(ml));

				ml.method_id = iJIT_GetNewMethodID();
				ml.method_name = (char*)name.c_str();
				ml.method_load_address = (void*)cg->getCode();
				ml.method_size = (unsigned int)cg->getSize();

				iJIT_NotifyEvent(iJVM_EVENT_TYPE_METHOD_LOAD_FINISHED, &ml);
/*
				name = format("c:/temp1/%s_%016llx.bin", m_name.c_str(), (uint64)key);

				if(FILE* fp = fopen(name.c_str(), "wb"))
				{
					fputc(0x0F, fp); fputc(0x0B, fp);
					fputc(0xBB, fp); fputc(0x6F, fp); fputc(0x00, fp); fputc(0x00, fp); fputc(0x00, fp);
					fputc(0x64, fp); fputc(0x67, fp); fputc(0x90, fp);

					fwrite(cg->getCode(), cg->getSize(), 1, fp);

					fputc(0xBB, fp); fputc(0xDE, fp); fputc(0x00, fp); fputc(0x00, fp); fputc(0x00, fp);
					fputc(0x64, fp); fputc(0x67, fp); fputc(0x90, fp);
					fputc(0x0F, fp); fputc(0x0B, fp);

					fclose(fp);
				}
*/
			}

#endif

			delete cg;
		}

		return ret;
	}
};
