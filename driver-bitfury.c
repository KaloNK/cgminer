/*
 * device-bitfury.c - device functions for Bitfury chip/board library
 *
 * Copyright (c) 2013 bitfury
 * Copyright (c) 2013 legkodymov
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
*/

#include "miner.h"
#include <unistd.h>
#include <sha2.h>
#include "libbitfury.h"
#include "util.h"
#include "config.h"

#include "bitfury-config.h"

struct device_drv bitfury_drv;

static int scan_delay = 0;
static int short_out_t = 0;
static int long_out_t = 0;


double shares_to_ghashes(int shares, int seconds) {
	return ( (double)shares * 4.294967296 ) / ( (double)seconds );

}

int calc_stat(time_t * stat_ts, time_t stat, struct timeval now) {
	int j;
	int shares_found = 0;
	for(j = 0; j < BITFURY_STAT_N; j++) {
		if (now.tv_sec - stat_ts[j] < stat) {
			shares_found++;
		}
	}
	return shares_found;
}

static void bitfury_detect(void)
{
	int chip_n;
	int i;
	struct cgpu_info *bitfury_info;

	bitfury_info = calloc(1, sizeof(struct cgpu_info));
	bitfury_info->drv = &bitfury_drv;
	bitfury_info->threads = 1;

	applog(LOG_INFO, "INFO: bitfury_detect");
	chip_n = libbitfury_detectChips(bitfury_info->devices);
	if (!chip_n) {
		applog(LOG_WARNING, "No Bitfury chips detected!");
		return;
	} else {
		applog(LOG_WARNING, "BITFURY: %d chips detected!", chip_n);
	}

	bitfury_info->chip_n = chip_n;
	add_cgpu(bitfury_info);
}

static uint32_t bitfury_checkNonce(struct work *work, uint32_t nonce)
{
	applog(LOG_INFO, "INFO: bitfury_checkNonce");
}

static int bitfury_submitNonce(struct thr_info *thr, struct bitfury_device *device, struct timeval *now, struct work *owork, uint32_t nonce)
{
	int i;

	for(i=0; i<(BITFURY_MAXCHIPS*4); i++) {
		if(device->nonces[i] == nonce) {
			return 0;
		}
	}

	submit_nonce(thr, owork, nonce);
	device->nonces[device->current_nonce++] = nonce;
	if(device->current_nonce > (BITFURY_MAXCHIPS*4)) {
		device->current_nonce = 0;
	}
	device->stat_ts[device->stat_counter++] = now->tv_sec;
	if (device->stat_counter == BITFURY_STAT_N) {
		device->stat_counter = 0;
	}

	return 1;
}

static double deviation_percents(double actual, double expected)
{
	return ((actual - expected) / expected * 100);
}

static scan_delay_normalize(int chip_n)
{
	if (chip_n < 9) {
		scan_delay <<= 4;
	} else if (chip_n < 17) {
		scan_delay <<= 3;
	} else if (chip_n < 33) {
		scan_delay <<= 2;
	} else if (chip_n < 65) {
		scan_delay <<= 1;
	}
}

static void bitfury_show_stats(struct bitfury_device *devices, struct timeval *now, int chip_n, int *out_t, int stat_interval, int log_prio)
{
	if (now->tv_sec - *out_t > stat_interval) {
		struct bitfury_device *dev;
		unsigned char line[512];
		int shares_first = 0, shares_last = 0, shares_total = 0;
		char stat_lines[BITFURY_MAXBANKS][512] = {0};
		int chip, len, i, k;
		double gh[BITFURY_MAXBANKS][BITFURY_BANKCHIPS][3] = {0};
		double total_expgh = 0, total_expgh2 = 0;

#ifdef BITFURY_LONG_STATS
		if (stat_interval == BITFURY_LONG_STATS) {
			sprintf(line, "!!!_________ LONG stat %ds: ___________!!!", BITFURY_LONG_STATS);
		} else
#endif
			sprintf(line, "vvvvwww SHORT stat %ds: wwwvvvv", stat_interval);

		applog(log_prio, "");
		applog(log_prio, line);

		for (chip = 0; chip < chip_n; chip++) {
			dev = &devices[chip];
			int shares_found = calc_stat(dev->stat_ts, stat_interval, *now);
			double ghash;

			len = strlen(stat_lines[dev->slot]);
			ghash = shares_to_ghashes(shares_found, stat_interval);
			gh[dev->slot][chip % BITFURY_BANKCHIPS][0] = ghash;
			if (dev->mhz) {
				double expected_ghash, expected_ghash2;

				gh[dev->slot][chip % BITFURY_BANKCHIPS][1] = expected_ghash = dev->mhz * 0.765 / 65;
				gh[dev->slot][chip % BITFURY_BANKCHIPS][2] = expected_ghash = dev->mhz * 0.864 / 65;
				snprintf(stat_lines[dev->slot] + len, 512 - len, "\n	Chip %3d	%.2f Gh/s @ %3.0f MHz (%d bits)		= %.2f Gh/s %+05.1f%% (rev 2 %.2f Gh/s %+05.1f%%) ",
					chip, ghash, dev->mhz, dev->osc6_bits, expected_ghash, deviation_percents(ghash, expected_ghash), expected_ghash2, deviation_percents(ghash, expected_ghash2));
			} else {
				gh[dev->slot][chip % BITFURY_BANKCHIPS][1] = gh[dev->slot][chip % BITFURY_BANKCHIPS][2] = 0;
				snprintf(stat_lines[dev->slot] + len, 512 - len, "\n	Chip %3d	%.2f Gh/s @ %3.0f MHz (%d bits)", chip, ghash, dev->mhz, dev->osc6_bits);
			}

			if(*out_t && ghash < 0.5) {
				applog(log_prio, "Chip_id %d FREQ REINIT", chip);
				send_freq(dev->slot, dev->fasync, dev->osc6_bits - 1);
				nmsleep(1);
				send_reinit(dev->slot, dev->fasync, dev->osc6_bits);
			}
			shares_total += shares_found;
			shares_first += chip < BITFURY_BANKCHIPS/2 ? shares_found : 0;
			shares_last += chip >= BITFURY_BANKCHIPS/2 ? shares_found : 0;

			if ( strlen(stat_lines[dev->slot]) > 200 || !((chip + 1) % BITFURY_BOARDCHIPS) ) {
				applog(log_prio, stat_lines[dev->slot]);
				stat_lines[dev->slot][0] = ' ';
				stat_lines[dev->slot][1] = 0;
				len = 1;
			}
		}

		for (i = 0; i < BITFURY_MAXBANKS; i++) {
			len = strlen(stat_lines[i]);
			if(len) {
				double expghsum = 0, expghsum2 = 0, ghsum = 0, gh1h = 0, gh2h = 0;

				for(k = 0; k < BITFURY_BOARDCHIPS; k++) {	// ToDo make it work for more than just 2 boards
					gh1h += gh[i][k][0];
					gh2h += gh[i][k + BITFURY_BOARDCHIPS][0];
					ghsum += gh[i][k][0] + gh[i][k + BITFURY_BOARDCHIPS][0];
					expghsum += gh[i][k][1] + gh[i][k + BITFURY_BOARDCHIPS][1];
					expghsum2 += gh[i][k][2] + gh[i][k + BITFURY_BOARDCHIPS][2];
				}
				if (ghsum) {
					total_expgh += expghsum;
					total_expgh2 += expghsum2;
					snprintf(stat_lines[i] + len, 512 - len, "\n	Slot %i	%2.2f Gh/s + %2.2f Gh/s = %2.2f Gh/s	= %.2f Gh/s %+05.1f%%%% (rev 2 %.2f Gh/s %+05.1f%%%%) ",
						i, gh1h, gh2h, ghsum, expghsum, deviation_percents(ghsum, expghsum), expghsum2, deviation_percents(ghsum, expghsum2));
				} else {
					snprintf(stat_lines[i] + len, 512 - len, "\n	Slot %i	%2.2f Gh/s + %2.2f Gh/s = %2.2f Gh/s", i, gh1h, gh2h, ghsum);
				}
				applog(log_prio, stat_lines[i]);
			}
		}
		applog(log_prio, "Total expected: %0.2f Gh/s (%0.2f Gh/s per chip)", total_expgh, total_expgh/chip_n);
		applog(log_prio, "Total expected rev 2: %0.2f Gh/s (%0.2f Gh/s per chip)", total_expgh2, total_expgh2/chip_n);
		*out_t = now->tv_sec;
		if (total_expgh) {
			scan_delay = 1000 * chip_n / (total_expgh2 / chip_n);
			scan_delay_normalize(chip_n);
		}
	}
}

static int64_t bitfury_scanHash(struct thr_info *thr)
{
	struct bitfury_device *devices, *dev;
	int chip, chip_n;
	uint64_t hashes = 0;
	struct timeval now, scan_end;
	int i;
	int nonces_cnt;

	devices = thr->cgpu->devices;
	chip_n = thr->cgpu->chip_n;

	if (!scan_delay) {
		for (i = 0; i < chip_n; i++) {
			devices[i].osc6_bits = devices[i].osc6_bits_setpoint;
			devices[i].osc6_req = devices[i].osc6_bits_setpoint;
		}
		for (i = 0; i < chip_n; i++) {
			send_reinit(devices[i].slot, devices[i].fasync, devices[i].osc6_bits);
		}
		scan_delay = 1000 * chip_n / 2.5;
		scan_delay_normalize(chip_n);
	}

	for (chip = 0; chip < chip_n; chip++) {
		dev = &devices[chip];
		if (dev->osc6_bits != dev->osc6_req) {
			dev->osc6_bits = dev->osc6_req;
			send_freq(dev->slot, dev->fasync, dev->osc6_bits);
			nmsleep(1);
			send_reinit(dev->slot, dev->fasync, dev->osc6_bits);
		}

		dev->job_switched = 0;
		if(!dev->work) {
			dev->work = get_queued(thr->cgpu);
			if (dev->work == NULL) {
				return 0;
			}
			work_to_payload(&(dev->payload), dev->work);
		}
	}

	libbitfury_sendHashData(thr, devices, chip_n);

	cgtime(&now);

	chip = 0;
	for (;chip < chip_n; chip++) {
		nonces_cnt = 0;
		dev = &devices[chip];
		if (dev->job_switched) {
			int *res = dev->results;
			struct work *work = dev->work;
			struct work *owork = dev->owork;
			struct work *o2work = dev->o2work;
			for (i = dev->results_n-1; i >= 0; i--) {
				if (owork) {
					nonces_cnt += bitfury_submitNonce(thr, dev, &now, owork, bswap_32(res[i]));
				}
				if (o2work) {
					// TEST
					//submit_nonce(thr, owork, bswap_32(res[j]));
				}
			}
			dev->results_n = 0;
			dev->job_switched = 0;
			if (dev->old_nonce && o2work)
				nonces_cnt += bitfury_submitNonce(thr, dev, &now, o2work, bswap_32(dev->old_nonce));

			if (dev->future_nonce)
				nonces_cnt += bitfury_submitNonce(thr, dev, &now, work, bswap_32(dev->future_nonce));

			if (o2work)
				work_completed(thr->cgpu, o2work);

			dev->o2work = dev->owork;
			dev->owork = dev->work;
			dev->work = NULL;
			hashes += 0xffffffffull * nonces_cnt;
			dev->matching_work += nonces_cnt;
		}
	}
#ifdef BITFURY_SHORT_STATS
	bitfury_show_stats(devices, &now, chip_n, &short_out_t, BITFURY_SHORT_STATS, LOG_NOTICE);
#else
	bitfury_show_stats(devices, &now, chip_n, &short_out_t, 60, LOG_DEBUG);
#endif
#ifdef BITFURY_LONG_STATS
	bitfury_show_stats(devices, &now, chip_n, &long_out_t, BITFURY_LONG_STATS, LOG_NOTICE);
#endif
	cgtime(&scan_end);
	i = scan_end.tv_usec - now.tv_usec;
	if (i<0) {
		i += 1000000;
	}

	if (i > scan_delay) {
		applog(LOG_WARNING, "Scan took %d us with scan interval %d", i, scan_delay);
		i = 0.9 * scan_delay;	// sleep 1/10 of the interval
	}

	nmsleep((scan_delay - i)/1000);

	return hashes;
}

static void bitfury_statline_before(char *buf, struct cgpu_info *cgpu)
{
	applog(LOG_INFO, "INFO bitfury_statline_before");
}

static void bitfury_shutdown(struct thr_info *thr)
{
	int chip_n;
	int i;

	chip_n = thr->cgpu->chip_n;

	applog(LOG_INFO, "INFO bitfury_shutdown");
	libbitfury_shutdownChips(thr->cgpu->devices, chip_n);
}

static void bitfury_disable(struct thr_info *thr)
{
	applog(LOG_INFO, "INFO bitfury_disable");
}

static int bitfury_findChip(struct bitfury_device *devices, int chip_n, int slot, int fs) {
	int n;
	for (n = 0; n < chip_n; n++) {
		if ( (devices[n].slot == slot) && (devices[n].fasync == fs) )
			return n;
	}
	return -1;
}

static void get_options(struct cgpu_info *cgpu)
{
	char buf[BUFSIZ+1];
	char *ptr, *comma, *colon, *colon2;
	size_t max = 0;
	int i, slot, fs, bits, chip, def_bits;

	for(i=0; i<cgpu->chip_n; i++)
		cgpu->devices[i].osc6_bits_setpoint = CLK_BITS_DEF; // this is default value

	if (opt_bitfury_clockbits == NULL) {
		buf[0] = '\0';
		return;
	}

	ptr = opt_bitfury_clockbits;

	do {
		comma = strchr(ptr, ',');
		if (comma == NULL)
			max = strlen(ptr);
		else
			max = comma - ptr;
		if (max > BUFSIZ)
			max = BUFSIZ;
		strncpy(buf, ptr, max);
		buf[max] = '\0';

		if (*buf) {
			colon = strchr(buf, ':');
			if (colon) {
				*(colon++) = '\0';
				colon2 = strchr(colon, ':');
				if (colon2) {
					*(colon2++) = '\0';

					slot = atoi(buf);
					fs = atoi(colon);
					bits = atoi(colon2);
					chip = bitfury_findChip(cgpu->devices, cgpu->chip_n, slot, fs);
					if(chip >= 0 && chip < cgpu->chip_n && bits >= CLK_BITS_MIN && bits <= CLK_BITS_MAX) {
						cgpu->devices[chip].osc6_bits_setpoint = bits;
						applog(LOG_INFO, "Set clockbits: slot=%d chip=%d bits=%d", slot, fs, bits);
					}
				}
			} else {
				def_bits = atoi(buf);
				if(def_bits >= CLK_BITS_MIN && def_bits <= CLK_BITS_MAX) {
					for(i=0; i<cgpu->chip_n; i++)
						cgpu->devices[i].osc6_bits_setpoint = def_bits;
				}
			}
		}
		if(comma != NULL)
			ptr = ++comma;
	} while (comma != NULL);
}

static bool bitfury_prepare(struct thr_info *thr)
{
	struct timeval now;
	struct cgpu_info *cgpu = thr->cgpu;

	cgtime(&now);
	get_datestamp(cgpu->init, &now);

	get_options(cgpu);

	applog(LOG_INFO, "INFO bitfury_prepare");
	return true;
}

static struct api_data *bitfury_api_stats(struct cgpu_info *cgpu)
{
	struct api_data *root = NULL;
	static struct bitfury_device *devices;
	struct timeval now;
	struct bitfury_info *info = cgpu->device_data;
	int shares_found, i;
	double ghash, ghash_sum = 0.0;
	char mcw[24];
	uint64_t total_hw = 0;

	devices = cgpu->devices;
	root = api_add_int(root, "chip_n", &(cgpu->chip_n),false);
	cgtime(&now);

	for (i = 0; i < cgpu->chip_n; i++) {
		sprintf(mcw, "clock_bits_%d_%d", devices[i].slot, devices[i].fasync);
		root = api_add_int(root, mcw, &(devices[i].osc6_bits), false);
	}
	for (i = 0; i < cgpu->chip_n; i++) {
		sprintf(mcw, "match_work_count_%d_%d", devices[i].slot, devices[i].fasync);
		root = api_add_uint(root, mcw, &(devices[i].matching_work), false);
	}
	for (i = 0; i < cgpu->chip_n; i++) {
		if (total_secs < 30) {
			devices[i].hw_errors = 0;
		}
		sprintf(mcw, "hw_errors_%d_%d", devices[i].slot, devices[i].fasync);
		root = api_add_uint(root, mcw, &(devices[i].hw_errors), false);
		total_hw += devices[i].hw_errors;
	}
	for (i = 0; i < cgpu->chip_n; i++) {
		sprintf(mcw, "mhz_%d_%d", devices[i].slot, devices[i].fasync);
		root = api_add_double(root, mcw, &(devices[i].mhz), false);
	}
	for (i = 0; i < cgpu->chip_n; i++) {
		shares_found = calc_stat(devices[i].stat_ts, BITFURY_API_STATS, now);
		ghash = shares_to_ghashes(shares_found, BITFURY_API_STATS);
		ghash_sum += ghash;
		sprintf(mcw, "ghash_%d_%d", devices[i].slot, devices[i].fasync);
		root = api_add_double(root, mcw, &(ghash), true);
	}
	api_add_uint64(root, "total_hw", &(total_hw), true);
	api_add_double(root, "total_gh", &(ghash_sum), true);
	ghash_sum /= cgpu->chip_n;
	api_add_double(root, "avg_gh_per_chip", &(ghash_sum), true);

	return root;
}

struct device_drv bitfury_drv = {
	.drv_id = DRIVER_BITFURY,
	.dname = "bitfury",
	.name = "BITFURY",
	.drv_detect = bitfury_detect,
	.get_statline_before = bitfury_statline_before,
	.thread_prepare = bitfury_prepare,
	.scanwork = bitfury_scanHash,
	.thread_shutdown = bitfury_shutdown,
	.hash_work = hash_queued_work,
	.get_api_stats = bitfury_api_stats,
};

