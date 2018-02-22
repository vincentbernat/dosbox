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

static void opl2lpt_reset(struct parport *pport) {
	LOG_MSG("OPL2LPT: reset OPL2 chip");
	for(int i = 0; i < 256; i ++) {
		opl2lpt_lpt_write(pport, i, C1284_NINIT | C1284_NSTROBE | C1284_NSELECTIN);
		usleep(4);
		opl2lpt_lpt_write(pport, 0, C1284_NINIT | C1284_NSELECTIN);
		usleep(23);
	}
}

static struct parport *opl2lpt_init(std::string name) {
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
			opl2lpt_reset(pport);
			LOG_MSG("OPL2LPT: found parallel port %s", pport->name);
			ieee1284_free_ports(&parports);
			return pport;
		}
	}
	ieee1284_free_ports(&parports);
	LOG_MSG("OPL2LPT: cannot find parallel port %s", name.c_str());
	return nullptr;
}

namespace OPL2LPT {

	void Handler::WriteReg(Bit32u reg, Bit8u val) {
		LOG_MSG("OPL2LPT: cycles %" PRId32 ", on reg 0x%" PRIx32 ", write 0x%" PRIx8,
			CPU_Cycles, reg, val);
		opl2lpt_lpt_write(pport, val, C1284_NINIT | C1284_NSELECTIN);
	}

	Bit32u Handler::WriteAddr(Bit32u port, Bit8u val) {
		LOG_MSG("OPL2LPT: cycles %" PRId32 ", on port 0x%" PRIx32 ", write 0x%" PRIx8,
			CPU_Cycles, port, val);
		opl2lpt_lpt_write(pport, val, C1284_NINIT | C1284_NSTROBE | C1284_NSELECTIN);
		return val;
	}

	void Handler::Generate(MixerChannel* chan, Bitu samples) {
		/* No need to generate sound */
		chan->enabled = false;
		return;
	}

	void Handler::Init(Bitu rate) {
		pport = opl2lpt_init(pportName);
	}

	Handler::~Handler() {
		opl2lpt_reset(pport);
		opl2lpt_shutdown(pport);
	}

};

#endif
