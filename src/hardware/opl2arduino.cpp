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

#include <math.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <termios.h>
#include <errno.h>

#include "adlib.h"
#include "dosbox.h"
#include "cpu.h"
#include "opl2arduino.h"

static void opl2arduino_shutdown(int port) {
	if (port != -1) {
		close(port);
	}
}

static void opl2arduino_write(int port, Bit8u d, Bit8u c) {
	if (port != -1) {
		/* The command format is:
		 *   - byte 1: register
		 *   - byte 2: value
		 *
		 * No delay is needed because ScummVM takes care of them and
		 * the code running on the Arduino already sleeps for 3.3 µs
		 * and 24 µs. */
		Bit8u cmd[] = {d, c};
		write(port, cmd, sizeof(cmd));
	}
}

static bool opl2arduino_ack(int port) {
	if (port != -1) {
		Bit8u ack;
		ssize_t n;
		n = read(port, &ack, 1);
		if (n == 1) {
			if (ack != 'k') {
				LOG_MSG("OPL2Arduino: expected ack, got 0x%02hhx", ack);
				return false;
			}
			return true;
		}

		/* Maybe the serial port was closed? */
		if (n == 0 || (n == -1 && errno == EBADF)) {
			LOG_MSG("OPL2Arduino: terminating ack thread");
			return false;
		}

		/* Other errors */
		LOG_MSG("OPL2Arduino: cannot get ack: %s",
			strerror(errno));
		return false;
	}
	return true;
}

static int opl2arduino_ack_thread(void *ptr) {
	OPL2Arduino::Handler *self = static_cast<OPL2Arduino::Handler *>(ptr);
	return self->AckThread();
}

static void opl2arduino_reset(int port) {
	LOG_MSG("OPL2Arduino: reset OPL2 chip");
	if (port != -1) {
		Bit8u cmd[] = {0, 0};
		write(port, cmd, sizeof(cmd));
	}
}

static bool opl2arduino_configure_serial(const char *name) {
	struct termios tty = {};
	int fd = open(name, O_RDWR);
	if (fd == -1) {
		LOG_MSG("OPL2Arduino: cannot open serial port: %s", strerror(errno));
		return false;
	}
	if (tcgetattr(fd, &tty) != 0) {
		LOG_MSG("OPL2Arduino: cannot get serial port attributes: %s", strerror(errno));
		close(fd);
		return false;
	}
	cfsetospeed(&tty, B115200);
	cfsetispeed(&tty, B115200);

	tty.c_cflag |= (CLOCAL | CREAD);    /* ignore modem controls */
	tty.c_cflag &= ~CSIZE;
	tty.c_cflag |= CS8;         /* 8-bit characters */
	tty.c_cflag &= ~PARENB;     /* no parity bit */
	tty.c_cflag &= ~CSTOPB;     /* only need 1 stop bit */
	tty.c_cflag &= ~CRTSCTS;    /* no hardware flowcontrol */

	/* setup for non-canonical mode */
	tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
	tty.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
	tty.c_oflag &= ~OPOST;

	/* fetch bytes as they become available */
	tty.c_cc[VMIN] = 1;
	tty.c_cc[VTIME] = 1;

	if (tcsetattr(fd, TCSANOW, &tty) != 0) {
		LOG_MSG("OPL2Arduino: cannot set serial port attributes: %s", strerror(errno));
		close(fd);
		return false;
	}
	close(fd);
	return true;
}

static int opl2arduino_init(const char *name, int *max_write) {
	int fd;
	char cmd[5];
	char digit;

	opl2arduino_configure_serial(name);
	fd = open(name, O_RDWR);
	if (fd == -1) {
		LOG_MSG("OPL2Arduino: cannot open serial port \"%s\": %s",
			name, strerror(errno));
		return -1;
	}

	// Wait for HLO!
	if (read(fd, cmd, sizeof(cmd)) != sizeof(cmd)) {
		LOG_MSG("OPL2Arduino: short read (1): %s", strerror(errno));
		goto error;
	}
	if (strncmp("HLO!\n", cmd, sizeof(cmd))) {
		LOG_MSG("OPL2Arduino: expected `HLO!', got `%s'", cmd);
		goto error;
	}

	// Send B0F? and wait for buffer size
	*max_write = 0;
	write(fd, "B0F?\n", 5);
	while (true) {
		if (read(fd, &digit, 1) != 1) {
			LOG_MSG("OPL2Arduino: short read (2): %s",
				strerror(errno));
			goto error;
		}
		if (digit >= '0' && digit <= '9') {
			*max_write *= 10;
			*max_write += digit - '0';
		} else if (digit == '\n') {
			break;
		} else {
			LOG_MSG("OPL2Arduino: cannot read buffer size (got 0x%02hhx)",
				digit);
			goto error;
		}
	}
	*max_write /= 2; // maximum number of commands we can pipeline
	LOG_MSG("OPL2Arduino: buffer is %d commands", *max_write);
	return fd;
error:
	close(fd);
	return -1;
}

namespace OPL2Arduino {

	void Handler::WriteReg(Bit32u reg, Bit8u val) {
		if (SDL_SemTryWait(sem) == SDL_MUTEX_TIMEDOUT) {
			LOG_MSG("OPL2Arduino: too slow, consider increasing buffer");
			SDL_SemWait(sem);
		}
		opl2arduino_write(port, index, val);
	}

	Bit32u Handler::WriteAddr(Bit32u port, Bit8u val) {
		index = val;
		return val;
	}

	int Handler::AckThread() {
		while (true) {
			if (!opl2arduino_ack(port)) {
				LOG_MSG("OPL2Arduino: end of ack thread");
				break;
			}
			SDL_SemPost(sem);
		}
		return 0;
	}

	void Handler::Generate(MixerChannel* chan, Bitu samples) {
		/* No need to generate sound */
		chan->enabled = false;
		return;
	}

	void Handler::Init(Bitu rate) {
		int max_write = 0;
		index = 0;
		port = opl2arduino_init(portName.c_str(), &max_write);
		if (port != -1) {
			sem = SDL_CreateSemaphore(max_write);
			if (!sem) {
				LOG_MSG("OPL2Arduino: unable to create semaphore: %s",
					SDL_GetError());
				opl2arduino_shutdown(port);
				port = -1;
			}
			thread = SDL_CreateThread(opl2arduino_ack_thread, this);
			if (!thread) {
				LOG_MSG("OPL2Arduino: unable to create ack thread: %s",
					SDL_GetError());
				opl2arduino_shutdown(port);
				port = -1;
			}
		}
	}

	Handler::~Handler() {
		opl2arduino_reset(port);
		opl2arduino_shutdown(port);
		if (thread) SDL_WaitThread(thread, nullptr);

		// Display last semaphore value. If different of
		// buffer size, we may have lost some acks.
		if (sem) {
			LOG_MSG("OPL2Arduino: last sem value is %d", SDL_SemValue(sem));
			SDL_DestroySemaphore(sem);
		}
	}

};
