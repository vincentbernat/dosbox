/*
 *  Copyright (C) 2002-2017  The DOSBox Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include <ieee1284.h>
#include <math.h>
#include <unistd.h>

#include "adlib.h"
#include "dosbox.h"
#include "cpu.h"
#include "opl2lpt.h"

#if C_OPL2LPT

static void opl2lpt_shutdown(struct parport *pport) {
	if (pport) {
		ieee1284_close(pport);
	}
}

static void opl2lpt_lpt_write(struct parport *pport, Bit8u d, Bit8u c) {
	if (pport) {
		ieee1284_write_data(pport, d);
		ieee1284_write_control(pport, c ^ C1284_INVERTED);
		c ^= C1284_NINIT;
		ieee1284_write_control(pport, c ^ C1284_INVERTED);
		c ^= C1284_NINIT;
		ieee1284_write_control(pport, c ^ C1284_INVERTED);
	}
}

static void opl2lpt_reset(struct parport *pport, Adlib::Mode mode) {
	LOG_MSG("OPL2LPT: reset %s chip", mode == Adlib::MODE_OPL2 ? "OPL2" : "OPL3");
	for (int i = 0; i < 256; i ++) {
		opl2lpt_lpt_write(pport, i, C1284_NINIT | C1284_NSTROBE | C1284_NSELECTIN);
		if (mode == Adlib::MODE_OPL2) usleep(4);
		opl2lpt_lpt_write(pport, 0, C1284_NINIT | C1284_NSELECTIN);
		if (mode == Adlib::MODE_OPL2) usleep(23);
	}
	if (mode == Adlib::MODE_OPL3) {
		for (int i = 0; i < 256; i ++) {
			opl2lpt_lpt_write(pport, i, C1284_NINIT | C1284_NSTROBE);
			opl2lpt_lpt_write(pport, 0, C1284_NINIT | C1284_NSELECTIN);
		}
	}
}

static struct parport *opl2lpt_init(std::string name, Adlib::Mode mode) {
	struct parport_list parports = {};
	struct parport *pport;

	// Look for available parallel ports
	if (ieee1284_find_ports(&parports, 0) != E1284_OK) {
		LOG_MSG("OPL2LPT: cannot find parallel ports");
		return nullptr;
	}
	for (int i = 0; i < parports.portc; i++) {
		if (name == "" ||
		    name == parports.portv[i]->name) {
			int caps = CAP1284_RAW;
			pport = parports.portv[i];
			if (ieee1284_open(pport, F1284_EXCL, &caps) != E1284_OK) {
				LOG_MSG("OPL2LPT: cannot open parallel port %s", pport->name);
			}
			if (ieee1284_claim(pport) != E1284_OK) {
				LOG_MSG("OPL2LPT: cannot claim parallel port %s", pport->name);
				ieee1284_close(pport);
				continue;
			}
			opl2lpt_reset(pport, mode);
			LOG_MSG("OPL2LPT: found parallel port %s", pport->name);
			ieee1284_free_ports(&parports);
			return pport;
		}
	}
	ieee1284_free_ports(&parports);
	LOG_MSG("OPL2LPT: cannot find parallel port %s", name.c_str());
	return nullptr;
}

static int opl2lpt_thread(void *ptr) {
	OPL2LPT::Handler *self = static_cast<OPL2LPT::Handler *>(ptr);
	self->WriteThread();
}

namespace OPL2LPT {

	const Bit16u EventQuit = 0x1000;
	const Bit16u EventAddr = 0x2000;
	const Bit16u EventReg  = 0x3000;

	void Handler::WriteReg(Bit32u reg, Bit8u val) {
#if C_DEBUG
		LOG_MSG("OPL2LPT: cycles %" PRId32 ", on reg 0x%" PRIx32 ", write 0x%" PRIx8,
			CPU_Cycles, reg, val);
#endif
		SDL_LockMutex(lock);
		eventQueue.push(EventReg | val);
		SDL_CondSignal(cond);
		SDL_UnlockMutex(lock);
	}

	Bit32u Handler::WriteAddr(Bit32u port, Bit8u val) {
#if C_DEBUG
		LOG_MSG("OPL2LPT: cycles %" PRId32 ", on port 0x%" PRIx32 ", write 0x%" PRIx8,
			CPU_Cycles, port, val);
#endif
		val |= ((port << 7) & 0x100);
		SDL_LockMutex(lock);
		eventQueue.push(EventAddr | val);
		SDL_CondSignal(cond);
		SDL_UnlockMutex(lock);
		return val;
	}

	void Handler::Generate(MixerChannel* chan, Bitu samples) {
		/* No need to generate sound */
		chan->enabled = false;
		return;
	}

	int Handler::WriteThread() {
		struct parport *pport = opl2lpt_init(pportName, mode);
		Bit16u event;

		while (true) {
			SDL_LockMutex(lock);
			while (eventQueue.empty()) {
				SDL_CondWait(cond, lock);
			}
			event = eventQueue.front();
			eventQueue.pop();
			SDL_UnlockMutex(lock);

			if ((event & 0xf000) == EventQuit) {
				LOG_MSG("OPL2LPT: quit sound thread");
				break;
			}
			switch (event & 0xf000) {
			case EventAddr:
				if (event & 0xfff < 0x100)
					opl2lpt_lpt_write(pport, event & 0xff,
							  C1284_NINIT | C1284_NSTROBE | C1284_NSELECTIN);
				else if (mode == Adlib::MODE_OPL3)
					opl2lpt_lpt_write(pport, event & 0xff,
							  C1284_NINIT | C1284_NSTROBE);
				if (mode == Adlib::MODE_OPL2) usleep(4);
				break;
			case EventReg:
				opl2lpt_lpt_write(pport, event & 0xff,
						  C1284_NINIT | C1284_NSELECTIN);
				if (mode == Adlib::MODE_OPL2) usleep(23);
				break;
			default:
				LOG_MSG("OPL2LPT: got unknown event 0x%" PRIx16, event);
			}
		}

		if (pport) {
			opl2lpt_reset(pport, mode);
			opl2lpt_shutdown(pport);
		}
		return 0;
	}

	void Handler::Init(Bitu rate) {
		thread = SDL_CreateThread(opl2lpt_thread, this);
		if (!thread) {
			LOG_MSG("OPL2LPT: unable to create thread: %s", SDL_GetError());
		}
	}

	Handler::Handler(std::string name, Adlib::Mode mode) : pportName(name), mode(mode), lock(SDL_CreateMutex()), cond(SDL_CreateCond()) {
	}

	Handler::~Handler() {
		SDL_LockMutex(lock);
		eventQueue.push(EventQuit);
		SDL_CondSignal(cond);
		SDL_UnlockMutex(lock);
		SDL_WaitThread(thread, nullptr);
	}

};

#endif
