/*---------------------------------------------------------------------------*\
                                                                             
  FILE........: fdmdv.c
  AUTHOR......: David Rowe
  DATE CREATED: April 14 2012
                                                                             
  Functions that implement the FDMDV modem.
                                                                             
\*---------------------------------------------------------------------------*/

/*
  Copyright (C) 2012 David Rowe

  All rights reserved.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License version 2.1, as
  published by the Free Software Foundation.  This program is
  distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or
  FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public
  License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with this program; if not, see <http://www.gnu.org/licenses/>.
*/

/*---------------------------------------------------------------------------*\
                                                                             
                               INCLUDES

\*---------------------------------------------------------------------------*/

#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "fdmdv_internal.h"
#include "fdmdv.h"
#include "rn.h"
#include "test_bits.h"
#include "pilot_coeff.h"
//#include "fft.h"
#include "hanning.h"
#include "os.h"
#include "arm_math.h"

#define OLD_QPSK_SYMBOLS

/*---------------------------------------------------------------------------*\
                                                                             
                               FUNCTIONS

\*---------------------------------------------------------------------------*/

static COMP cneg(COMP a)
{
    COMP res;

    res.real = -a.real;
    res.imag = -a.imag;

    return res;
}

static COMP cconj(COMP a)
{
    COMP res;

    res.real = a.real;
    res.imag = -a.imag;

    return res;
}

static COMP cmult(COMP a, COMP b)
{
    COMP res;

    res.real = a.real*b.real - a.imag*b.imag;
    res.imag = a.real*b.imag + a.imag*b.real;

    return res;
}

static COMP fcmult(float a, COMP b)
{
    COMP res;

    res.real = a*b.real;
    res.imag = a*b.imag;

    return res;
}

static COMP cadd(COMP a, COMP b)
{
    COMP res;

    res.real = a.real + b.real;
    res.imag = a.imag + b.imag;

    return res;
}

/*---------------------------------------------------------------------------*\
                                                       
  FUNCTION....: fdmdv_create	     
  AUTHOR......: David Rowe			      
  DATE CREATED: 16/4/2012 

  Create and initialise an instance of the modem.  Returns a pointer
  to the modem states or NULL on failure.  One set of states is
  sufficient for a full duplex modem.

\*---------------------------------------------------------------------------*/

struct FDMDV *fdmdv_create(void)
{
    struct FDMDV *f;
    int           c, i, k;
    float         carrier_freq;

    assert(FDMDV_BITS_PER_FRAME == NC*NB);
    assert(FDMDV_NOM_SAMPLES_PER_FRAME == M_OSF);
    assert(FDMDV_MAX_SAMPLES_PER_FRAME == (M_OSF+M_OSF/P_OSF));

    f = (struct FDMDV*)malloc(sizeof(struct FDMDV));
    if (f == NULL)
	return NULL;
    
    f->current_test_bit = 0;
    for(i=0; i<NTEST_BITS; i++)
	f->rx_test_bits_mem[i] = 0;

    f->tx_pilot_bit = 0;

  for(c=0; c<NC+1; c++) {
	f->prev_tx_symbols[c].real = 1.0;
	f->prev_tx_symbols[c].imag = 0.0;
#if 0
	f->prev_rx_symbols[c].real = 1.0;
	f->prev_rx_symbols[c].imag = 0.0;

	for(k=0; k<NFILTER; k++) {
	    f->rx_filter_memory[c][k].real = 0.0;
	    f->rx_filter_memory[c][k].imag = 0.0;
	}
#endif
	for(k=0; k<NSYM; k++) {
	    f->tx_filter_memory[c][k].real = 0.0;
	    f->tx_filter_memory[c][k].imag = 0.0;
	}

	/* Spread initial FDM carrier phase out as far as possible.
           This helped PAPR for a few dB.  We don't need to adjust rx
           phase as DQPSK takes care of that. */
	
	f->phase_tx[c].real = arm_cos_f32(2.0*PI*c/(NC+1));
 	f->phase_tx[c].imag = arm_sin_f32(2.0*PI*c/(NC+1));
#if 0
	f->phase_rx[c].real = 1.0;
 	f->phase_rx[c].imag = 0.0;

	for(k=0; k<NT*P_OSF; k++) {
	    f->rx_filter_mem_timing[c][k].real = 0.0;
	    f->rx_filter_mem_timing[c][k].imag = 0.0;
	}
 	for(k=0; k<NFILTERTIMING; k++) {
	    f->rx_baseband_mem_timing[c][k].real = 0.0;
	    f->rx_baseband_mem_timing[c][k].imag = 0.0;
	}
#endif
  }
    /* Set up frequency of each carrier */

    for(c=0; c<NC/2; c++) {
		carrier_freq = (-NC/2 + c)*FSEP + FCENTRE;
		f->freq[c].real = arm_cos_f32(2.0*PI*carrier_freq/FS);
		f->freq[c].imag = arm_sin_f32(2.0*PI*carrier_freq/FS);
    }

    for(c=NC/2; c<NC; c++) {
		carrier_freq = (-NC/2 + c + 1)*FSEP + FCENTRE;
		f->freq[c].real = arm_cos_f32(2.0*PI*carrier_freq/FS);
		f->freq[c].imag = arm_sin_f32(2.0*PI*carrier_freq/FS);
    }
	
    f->freq[NC].real = cos(2.0*PI*FCENTRE/FS);
    f->freq[NC].imag = sin(2.0*PI*FCENTRE/FS);

    /* Generate DBPSK pilot Look Up Table (LUT) */

    generate_pilot_lut(f->pilot_lut, &f->freq[NC]);

    /* freq Offset estimation states */

    for(i=0; i<NPILOTBASEBAND; i++) {
		f->pilot_baseband1[i].real = f->pilot_baseband2[i].real = 0.0;
		f->pilot_baseband1[i].imag = f->pilot_baseband2[i].imag = 0.0;
    }
    f->pilot_lut_index = 0;
    f->prev_pilot_lut_index = 3*M_OSF;
    
    for(i=0; i<NPILOTLPF; i++) {
		f->pilot_lpf1[i].real = f->pilot_lpf2[i].real = 0.0;
		f->pilot_lpf1[i].imag = f->pilot_lpf2[i].imag = 0.0;
    }

    f->foff = 0.0;
    f->foff_rect.real = 1.0;
    f->foff_rect.imag = 0.0;
    f->foff_phase_rect.real = 1.0;
    f->foff_phase_rect.imag = 0.0;

    f->fest_state = 0;
    f->coarse_fine = COARSE;

    return f;
}

/*---------------------------------------------------------------------------*\
                                                       
  FUNCTION....: fdmdv_destroy	     
  AUTHOR......: David Rowe			      
  DATE CREATED: 16/4/2012

  Destroy an instance of the modem.

\*---------------------------------------------------------------------------*/

void fdmdv_destroy(struct FDMDV *fdmdv)
{
    assert(fdmdv != NULL);
    free(fdmdv);
}

/*---------------------------------------------------------------------------*\
                                                       
  FUNCTION....: fdmdv_get_test_bits()	     
  AUTHOR......: David Rowe			      
  DATE CREATED: 16/4/2012

  Generate a frame of bits from a repeating sequence of random data.  OK so
  it's not very random if it repeats but it makes syncing at the demod easier
  for test purposes.

\*---------------------------------------------------------------------------*/

void fdmdv_get_test_bits(struct FDMDV *f, int tx_bits[])
{
    int i;

    for(i=0; i<FDMDV_BITS_PER_FRAME; i++) {
	tx_bits[i] = test_bits[f->current_test_bit];
	f->current_test_bit++;
	if (f->current_test_bit > (NTEST_BITS-1))
	    f->current_test_bit = 0;
    }
 }

/*---------------------------------------------------------------------------*\
                                                       
  FUNCTION....: bits_to_dqpsk_symbols()	     
  AUTHOR......: David Rowe			      
  DATE CREATED: 16/4/2012

  Maps bits to parallel DQPSK symbols. Generate Nc+1 QPSK symbols from
  vector of (1,Nc*Nb) input tx_bits.  The Nc+1 symbol is the +1 -1 +1
  .... BPSK sync carrier.

\*---------------------------------------------------------------------------*/

void bits_to_dqpsk_symbols(COMP tx_symbols[], COMP prev_tx_symbols[], int tx_bits[], int *pilot_bit)
{
    int c, msb, lsb;
    COMP j = {0.0,1.0};

    /* map tx_bits to to Nc DQPSK symbols */

    for(c=0; c<NC; c++) {
	msb = tx_bits[2*c]; 
	lsb = tx_bits[2*c+1];
	if ((msb == 0) && (lsb == 0))
	    tx_symbols[c] = prev_tx_symbols[c];
	if ((msb == 0) && (lsb == 1))
	    tx_symbols[c] = cmult(j, prev_tx_symbols[c]);
#ifdef OLD_QPSK_SYMBOLS
	if ((msb == 1) && (lsb == 0))
	    tx_symbols[c] = cneg(prev_tx_symbols[c]);
	if ((msb == 1) && (lsb == 1))
	    tx_symbols[c] = cmult(cneg(j),prev_tx_symbols[c]);
#else
	if ((msb == 1) && (lsb == 0))
	    tx_symbols[c] = cmult(cneg(j),prev_tx_symbols[c]);
	if ((msb == 1) && (lsb == 1))
	    tx_symbols[c] = cneg(prev_tx_symbols[c]);
#endif
    }

    /* +1 -1 +1 -1 BPSK sync carrier, once filtered becomes (roughly)
       two spectral lines at +/- Rs/2 */
 
    if (*pilot_bit)
	tx_symbols[NC] = cneg(prev_tx_symbols[NC]);
    else
	tx_symbols[NC] = prev_tx_symbols[NC];

    if (*pilot_bit) 
	*pilot_bit = 0;
    else
	*pilot_bit = 1;
}

/*---------------------------------------------------------------------------*\
                                                       
  FUNCTION....: tx_filter()	     
  AUTHOR......: David Rowe			      
  DATE CREATED: 17/4/2012

  Given NC*NB bits construct M_OSF samples (1 symbol) of NC+1 filtered
  symbols streams.

\*---------------------------------------------------------------------------*/

void tx_filter(COMP tx_baseband[NC+1][M_OSF], COMP tx_symbols[], COMP tx_filter_memory[NC+1][NSYM])
{
    int     c;
    int     i,j,k;
    float   acc;
    COMP    gain;

    gain.real = sqrtf(2.0)/2.0;
    gain.imag = 0.0;

    for(c=0; c<NC+1; c++)
	tx_filter_memory[c][NSYM-1] = cmult(tx_symbols[c], gain);

    /* 
       tx filter each symbol, generate M_OSF filtered output samples for each symbol.
       Efficient polyphase filter techniques used as tx_filter_memory is sparse
    */

    for(i=0; i<M_OSF; i++) {
	for(c=0; c<NC+1; c++) {

	    /* filter real sample of symbol for carrier c */

	    acc = 0.0;
	    for(j=0,k=M_OSF-i-1; j<NSYM; j++,k+=M_OSF)
		acc += M_OSF * tx_filter_memory[c][j].real * gt_alpha5_root[k];
	    tx_baseband[c][i].real = acc;	

	    /* filter imag sample of symbol for carrier c */

	    acc = 0.0;
	    for(j=0,k=M_OSF-i-1; j<NSYM; j++,k+=M_OSF)
		acc += M_OSF * tx_filter_memory[c][j].imag * gt_alpha5_root[k];
	    tx_baseband[c][i].imag = acc;

	}
    }

    /* shift memory, inserting zeros at end */

    for(i=0; i<NSYM-1; i++)
	for(c=0; c<NC+1; c++)
	    tx_filter_memory[c][i] = tx_filter_memory[c][i+1];

	for(c=0; c<NC+1; c++) {
	    tx_filter_memory[c][NSYM-1].real = 0.0;
	    tx_filter_memory[c][NSYM-1].imag = 0.0;
	}
}

/*---------------------------------------------------------------------------*\
                                                       
  FUNCTION....: fdm_upconvert()	     
  AUTHOR......: David Rowe			      
  DATE CREATED: 17/4/2012

  Construct FDM signal by frequency shifting each filtered symbol
  stream.  Returns complex signal so we can apply frequency offsets
  easily.

\*---------------------------------------------------------------------------*/

void fdm_upconvert(COMP tx_fdm[], COMP tx_baseband[NC+1][M_OSF], COMP phase_tx[], COMP freq[])
{
    int  i,c;
    COMP two = {2.0, 0.0};
    COMP pilot;

    for(i=0; i<M_OSF; i++) {
	tx_fdm[i].real = 0.0;
	tx_fdm[i].imag = 0.0;
    }

    /* Nc/2 tones below centre freq */
  
    for (c=0; c<NC/2; c++) 
	for (i=0; i<M_OSF; i++) {
	    phase_tx[c] = cmult(phase_tx[c], freq[c]);
	    tx_fdm[i] = cadd(tx_fdm[i], cmult(tx_baseband[c][i], phase_tx[c]));
	}

    /* Nc/2 tones above centre freq */

    for (c=NC/2; c<NC; c++) 
	for (i=0; i<M_OSF; i++) {
	    phase_tx[c] = cmult(phase_tx[c], freq[c]);
	    tx_fdm[i] = cadd(tx_fdm[i], cmult(tx_baseband[c][i], phase_tx[c]));
	}

    /* add centre pilot tone  */

    c = NC;
    for (i=0; i<M_OSF; i++) {
	phase_tx[c] = cmult(phase_tx[c],  freq[c]);
	pilot = cmult(cmult(two, tx_baseband[c][i]), phase_tx[c]);
	tx_fdm[i] = cadd(tx_fdm[i], pilot);
    }

    /*
      Scale such that total Carrier power C of real(tx_fdm) = Nc.  This
      excludes the power of the pilot tone.
      We return the complex (single sided) signal to make frequency
      shifting for the purpose of testing easier
    */

    for (i=0; i<M_OSF; i++) 
	tx_fdm[i] = cmult(two, tx_fdm[i]);

}

/*---------------------------------------------------------------------------*\
                                                       
  FUNCTION....: fdmdv_mod()	     
  AUTHOR......: David Rowe			      
  DATE CREATED: 26/4/2012

  FDMDV modulator, take a frame of FDMDV_BITS_PER_FRAME bits and
  generates a frame of FDMDV_SAMPLES_PER_FRAME modulated symbols.
  Sync bit is returned to aid alignment of your next frame.  The
  sync_bit value returned will be used for the _next_ frame.

\*---------------------------------------------------------------------------*/

void fdmdv_mod(struct FDMDV *fdmdv, COMP tx_fdm[], int tx_bits[], int *sync_bit)
{
    COMP          tx_symbols[NC+1];
    COMP          tx_baseband[NC+1][M_OSF];

    bits_to_dqpsk_symbols(tx_symbols, fdmdv->prev_tx_symbols, tx_bits, &fdmdv->tx_pilot_bit);
    memcpy(fdmdv->prev_tx_symbols, tx_symbols, sizeof(COMP)*(NC+1));
    tx_filter(tx_baseband, tx_symbols, fdmdv->tx_filter_memory);
    fdm_upconvert(tx_fdm, tx_baseband, fdmdv->phase_tx, fdmdv->freq);

    *sync_bit = fdmdv->tx_pilot_bit;
}

/*---------------------------------------------------------------------------*\
                                                       
  FUNCTION....: generate_pilot_fdm()	     
  AUTHOR......: David Rowe			      
  DATE CREATED: 19/4/2012

  Generate M_OSF samples of DBPSK pilot signal for Freq offset estimation.

\*---------------------------------------------------------------------------*/

void generate_pilot_fdm(COMP *pilot_fdm, int *bit, float *symbol, 
			float *filter_mem, COMP *phase, COMP *freq)
{
    int   i,j,k;
    float tx_baseband[M_OSF];

    /* +1 -1 +1 -1 DBPSK sync carrier, once filtered becomes (roughly)
       two spectral lines at +/- RS/2 */
 
    if (*bit)
    	*symbol = -*symbol;
    //else
    //	*symbol = *symbol;
    if (*bit) 
    	*bit = 0;
    else
    	*bit = 1;

    /* filter DPSK symbol to create M_OSF baseband samples */

    filter_mem[NFILTER-1] = (sqrtf(2)/2) * *symbol;
    for(i=0; i<M_OSF; i++) {
	tx_baseband[i] = 0.0; 
	for(j=M_OSF-1,k=M_OSF-i-1; j<NFILTER; j+=M_OSF,k+=M_OSF)
	    tx_baseband[i] += M_OSF * filter_mem[j] * gt_alpha5_root[k];
    }

    /* shift memory, inserting zeros at end */

    for(i=0; i<NFILTER-M_OSF; i++)
	filter_mem[i] = filter_mem[i+M_OSF];

    for(i=NFILTER-M_OSF; i<NFILTER; i++)
	filter_mem[i] = 0.0;

    /* upconvert */

    for(i=0; i<M_OSF; i++) {
	*phase = cmult(*phase, *freq);
	pilot_fdm[i].real = sqrtf(2)*2*tx_baseband[i] * phase->real;
	pilot_fdm[i].imag = sqrtf(2)*2*tx_baseband[i] * phase->imag;
    }
}

/*---------------------------------------------------------------------------*\
                                                       
  FUNCTION....: generate_pilot_lut()	     
  AUTHOR......: David Rowe			      
  DATE CREATED: 19/4/2012

  Generate a 4M sample vector of DBPSK pilot signal.  As the pilot signal
  is periodic in 4M samples we can then use this vector as a look up table
  for pilot signal generation in the demod.

\*---------------------------------------------------------------------------*/

void generate_pilot_lut(COMP pilot_lut[], COMP *pilot_freq)
{
    int   pilot_rx_bit = 0;
    float pilot_symbol = sqrt(2.0);
    COMP  pilot_phase  = {1.0, 0.0};
    float pilot_filter_mem[NFILTER];
    COMP  pilot[M_OSF];
    int   i,f;

    for(i=0; i<NFILTER; i++)
	pilot_filter_mem[i] = 0.0;

    /* discard first 4 symbols as filter memory is filling, just keep
       last four symbols */

    for(f=0; f<8; f++) {
	generate_pilot_fdm(pilot, &pilot_rx_bit, &pilot_symbol, pilot_filter_mem, &pilot_phase, pilot_freq);
	if (f >= 4)
	    memcpy(&pilot_lut[M_OSF*(f-4)], pilot, M_OSF*sizeof(COMP));
    }

}
#if 0
/*---------------------------------------------------------------------------*\
                                                       
  FUNCTION....: lpf_peak_pick()	     
  AUTHOR......: David Rowe			      
  DATE CREATED: 20/4/2012

  LPF and peak pick part of freq est, put in a function as we call it twice.

\*---------------------------------------------------------------------------*/

void lpf_peak_pick(float *foff, float *max, COMP pilot_baseband[], COMP pilot_lpf[], COMP S[], int nin)
{
    int   i,j,k;
    int   mpilot;
    float mag, imax;
    int   ix;
    float r;

    /* LPF cutoff 200Hz, so we can handle max +/- 200 Hz freq offset */

    for(i=0; i<NPILOTLPF-nin; i++)
	pilot_lpf[i] = pilot_lpf[nin+i];
    for(i=NPILOTLPF-nin, j=0; i<NPILOTLPF; i++,j++) {
	pilot_lpf[i].real = 0.0; pilot_lpf[i].imag = 0.0;
	for(k=0; k<NPILOTCOEFF; k++)
	    pilot_lpf[i] = cadd(pilot_lpf[i], fcmult(pilot_coeff[k], pilot_baseband[j+k]));
    }

    /* decimate to improve DFT resolution, window and DFT */

    mpilot = FS/(2*200);  /* calc decimation rate given new sample rate is twice LPF freq */
    for(i=0; i<MPILOTFFT; i++) {
	S[i].real = 0.0; S[i].imag = 0.0;
    }
    for(i=0,j=0; i<NPILOTLPF; i+=mpilot,j++) {
	S[j] = fcmult(hanning[i], pilot_lpf[i]);
    }

    fft(&S[0].real, MPILOTFFT, -1);

    /* peak pick and convert to Hz */

    imax = 0.0;
    ix = 0;
    for(i=0; i<MPILOTFFT; i++) {
	mag = S[i].real*S[i].real + S[i].imag*S[i].imag;
	if (mag > imax) {
	    imax = mag;
	    ix = i;
	}
    }
    r = 2.0*200.0/MPILOTFFT;     /* maps FFT bin to frequency in Hz */
  
    if (ix >= MPILOTFFT/2)
	*foff = (ix - MPILOTFFT)*r;
    else
	*foff = (ix)*r;
    *max = imax;

}

/*---------------------------------------------------------------------------*\
                                                       
  FUNCTION....: rx_est_freq_offset()	     
  AUTHOR......: David Rowe			      
  DATE CREATED: 19/4/2012

  Estimate frequency offset of FDM signal using BPSK pilot.  Note that
  this algorithm is quite sensitive to pilot tone level wrt other
  carriers, so test variations to the pilot amplitude carefully.

\*---------------------------------------------------------------------------*/

float rx_est_freq_offset(struct FDMDV *f, float rx_fdm[], int nin)
{
    int  i,j;
    COMP pilot[M_OSF+M_OSF/P_OSF];
    COMP prev_pilot[M_OSF+M_OSF/P_OSF];
    float foff, foff1, foff2;
    float   max1, max2;

    assert(nin <= M_OSF+M_OSF/P_OSF);

    /* get pilot samples used for correlation/down conversion of rx signal */

    for (i=0; i<nin; i++) {
	pilot[i] = f->pilot_lut[f->pilot_lut_index];
	f->pilot_lut_index++;
	if (f->pilot_lut_index >= 4*M_OSF)
	    f->pilot_lut_index = 0;
	
	prev_pilot[i] = f->pilot_lut[f->prev_pilot_lut_index];
	f->prev_pilot_lut_index++;
	if (f->prev_pilot_lut_index >= 4*M_OSF)
	    f->prev_pilot_lut_index = 0;
    }

    /*
      Down convert latest M_OSF samples of pilot by multiplying by ideal
      BPSK pilot signal we have generated locally.  The peak of the
      resulting signal is sensitive to the time shift between the
      received and local version of the pilot, so we do it twice at
      different time shifts and choose the maximum.
    */

    for(i=0; i<NPILOTBASEBAND-nin; i++) {
	f->pilot_baseband1[i] = f->pilot_baseband1[i+nin];
	f->pilot_baseband2[i] = f->pilot_baseband2[i+nin];
    }

    for(i=0,j=NPILOTBASEBAND-nin; i<nin; i++,j++) {
       	f->pilot_baseband1[j] = fcmult(rx_fdm[i], cconj(pilot[i]));
	f->pilot_baseband2[j] = fcmult(rx_fdm[i], cconj(prev_pilot[i]));
    }

    lpf_peak_pick(&foff1, &max1, f->pilot_baseband1, f->pilot_lpf1, f->S1, nin);
    lpf_peak_pick(&foff2, &max2, f->pilot_baseband2, f->pilot_lpf2, f->S2, nin);

    if (max1 > max2)
	foff = foff1;
    else
	foff = foff2;
	
    return foff;
}

/*---------------------------------------------------------------------------*\
                                                       
  FUNCTION....: freq_shift()	     
  AUTHOR......: David Rowe			      
  DATE CREATED: 26/4/2012

  Frequency shift modem signal.

\*---------------------------------------------------------------------------*/

void freq_shift(COMP rx_fdm_fcorr[], float rx_fdm[], float foff, COMP *foff_rect, COMP *foff_phase_rect, int nin)
{
    int i;

    foff_rect->real = arm_cos_f32(2.0*PI*foff/FS);
    foff_rect->imag = arm_sin_f32(2.0*PI*foff/FS);
    for(i=0; i<nin; i++) {
	*foff_phase_rect = cmult(*foff_phase_rect, cconj(*foff_rect));
	rx_fdm_fcorr[i] = fcmult(rx_fdm[i], *foff_phase_rect);
    }
  
}

/*---------------------------------------------------------------------------*\
                                                       
  FUNCTION....: fdm_downconvert()	     
  AUTHOR......: David Rowe			      
  DATE CREATED: 22/4/2012

  Frequency shift each modem carrier down to Nc+1 baseband signals.

\*---------------------------------------------------------------------------*/

void fdm_downconvert(COMP rx_baseband[NC+1][M_OSF+M_OSF/P_OSF], COMP rx_fdm[], COMP phase_rx[], COMP freq[], int nin)
{
    int  i,c;

    /* maximum number of input samples to demod */

    assert(nin <= (M_OSF+M_OSF/P_OSF));

    /* Nc/2 tones below centre freq */
  
    for (c=0; c<NC/2; c++) 
	for (i=0; i<nin; i++) {
	    phase_rx[c] = cmult(phase_rx[c], freq[c]);
	    rx_baseband[c][i] = cmult(rx_fdm[i], cconj(phase_rx[c]));
	}

    /* Nc/2 tones above centre freq */

    for (c=NC/2; c<NC; c++) 
	for (i=0; i<nin; i++) {
	    phase_rx[c] = cmult(phase_rx[c], freq[c]);
	    rx_baseband[c][i] = cmult(rx_fdm[i], cconj(phase_rx[c]));
	}

    /* centre pilot tone  */

    c = NC;
    for (i=0; i<nin; i++) {
	phase_rx[c] = cmult(phase_rx[c],  freq[c]);
	rx_baseband[c][i] = cmult(rx_fdm[i], cconj(phase_rx[c]));
    }

}

/*---------------------------------------------------------------------------*\
                                                       
  FUNCTION....: rx_filter()	     
  AUTHOR......: David Rowe			      
  DATE CREATED: 22/4/2012

  Receive filter each baseband signal at oversample rate P_OSF.  Filtering at
  rate P_OSF lowers CPU compared to rate M_OSF.

  Depending on the number of input samples to the demod nin, we
  produce P_OSF-1, P_OSF (usually), or P_OSF+1 filtered samples at rate P_OSF.  nin is
  occasionally adjusted to compensate for timing slips due to
  different tx and rx sample clocks.

\*---------------------------------------------------------------------------*/

void rx_filter(COMP rx_filt[NC+1][P_OSF+1], COMP rx_baseband[NC+1][M_OSF+M_OSF/P_OSF], COMP rx_filter_memory[NC+1][NFILTER], int nin)
{
    int c, i,j,k,l;

    /* rx filter each symbol, generate P_OSF filtered output samples for
       each symbol.  Note we keep filter memory at rate M_OSF, it's just
       the filter output at rate P_OSF */

    for(i=0, j=0; i<nin; i+=M_OSF/P_OSF,j++) {
	
		/* latest input sample */

		for(c=0; c<NC+1; c++)
			for(k=NFILTER-M_OSF/P_OSF,l=i; k<NFILTER; k++,l++)
			rx_filter_memory[c][k] = rx_baseband[c][l];
		//rx_filter_memory[0...14][6*160-160/4 ... 6*160] = rx_baseband[0...14][0|160/4|...|nin];
		//fill tail of rx_filter_memory[0...14] by 160/4 samples

		/* convolution (filtering) */
	
		for(c=0; c<NC+1; c++) {
			rx_filt[c][j].real = 0.0; rx_filt[c][j].imag = 0.0;
			//for(k=0; k<NFILTER; k++)
			//rx_filt[c][j] = cadd(rx_filt[c][j], fcmult(gt_alpha5_root[k], rx_filter_memory[c][k]));
			for(k=0; k<NFILTER; k++) {
				rx_filt[c][j].real += gt_alpha5_root[k] * rx_filter_memory[c][k].real;
				rx_filt[c][j].imag += gt_alpha5_root[k] * rx_filter_memory[c][k].imag;
			}
			//rx_filter_memory[0...14][0...6*160]*gt_alpha5_root[0...6*160]
		}
	
		/* make room for next input sample */

		for(c=0; c<NC+1; c++)
			for(k=0,l=M_OSF/P_OSF; k<NFILTER-M_OSF/P_OSF; k++,l++)
			rx_filter_memory[c][k] = rx_filter_memory[c][l];
    }
    //rx_filter_memory[0...14][0...6*160-160/4] = rx_filter_memory[0...14][160/4...6*160];
    // shift rx_filter_memory[0...14] << 160/4 samples

    assert(j <= (P_OSF+1)); /* check for any over runs */
}

/*---------------------------------------------------------------------------*\
                                                       
  FUNCTION....: rx_est_timing()	     
  AUTHOR......: David Rowe			      
  DATE CREATED: 23/4/2012

  Estimate optimum timing offset, re-filter receive symbols at optimum
  timing estimate.

\*---------------------------------------------------------------------------*/

float rx_est_timing(COMP rx_symbols[], 
		    COMP rx_filt[NC+1][P_OSF+1], 
		    COMP rx_baseband[NC+1][M_OSF+M_OSF/P_OSF], 
		    COMP rx_filter_mem_timing[NC+1][NT*P_OSF], 
		    float env[],
		    COMP rx_baseband_mem_timing[NC+1][NFILTERTIMING], 
		    int nin)	 
{
    int   c,i,j,k;
    int   adjust, s;
    COMP  x, phase, freq;
    float rx_timing;

    /*
      nin  adjust 
      --------------------------------
      120  -1 (one less rate P_OSF sample)
      160   0 (nominal)
      200   1 (one more rate P_OSF sample)
    */

    adjust = P_OSF - nin*P_OSF/M_OSF;
    
    /* update buffer of NT rate P_OSF filtered symbols */
    
    for(c=0; c<NC+1; c++) 
	for(i=0,j=P_OSF-adjust; i<(NT-1)*P_OSF+adjust; i++,j++)
	    rx_filter_mem_timing[c][i] = rx_filter_mem_timing[c][j];
    for(c=0; c<NC+1; c++) 
	for(i=(NT-1)*P_OSF+adjust,j=0; i<NT*P_OSF; i++,j++)
	    rx_filter_mem_timing[c][i] = rx_filt[c][j];
	    
    /* sum envelopes of all carriers */

    for(i=0; i<NT*P_OSF; i++) {
	env[i] = 0.0;
	for(c=0; c<NC+1; c++)
	    env[i] += sqrt(pow(rx_filter_mem_timing[c][i].real,2.0) + pow(rx_filter_mem_timing[c][i].imag,2.0));
    }

    /* The envelope has a frequency component at the symbol rate.  The
       phase of this frequency component indicates the timing.  So work
       out single DFT at frequency 2*pi/P_OSF */

    x.real = 0.0; x.imag = 0.0;
    freq.real = cos(2*PI/P_OSF);
    freq.imag = sin(2*PI/P_OSF);
    phase.real = 1.0;
    phase.imag = 0.0;

    for(i=0; i<NT*P_OSF; i++) {
	x = cadd(x, fcmult(env[i], phase));
	phase = cmult(phase, freq);
    }

    /* Map phase to estimated optimum timing instant at rate M_OSF.  The
       M_OSF/4 part was adjusted by experiment, I know not why.... */
    
    rx_timing = atan2(x.imag, x.real)*M_OSF/(2*PI) + M_OSF/4;
    
    if (rx_timing > M_OSF)
	rx_timing -= M_OSF;
    if (rx_timing < -M_OSF)
	rx_timing += M_OSF;
   
    /* rx_filt_mem_timing contains M_OSF + Nfilter + M_OSF samples of the
       baseband signal at rate M_OSF this enables us to resample the
       filtered rx symbol with M_OSF sample precision once we have
       rx_timing */

    for(c=0; c<NC+1; c++) 
	for(i=0,j=nin; i<NFILTERTIMING-nin; i++,j++)
	    rx_baseband_mem_timing[c][i] = rx_baseband_mem_timing[c][j];
    for(c=0; c<NC+1; c++) 
	for(i=NFILTERTIMING-nin,j=0; i<NFILTERTIMING; i++,j++)
	    rx_baseband_mem_timing[c][i] = rx_baseband[c][j];
    
    /* rx filter to get symbol for each carrier at estimated optimum
       timing instant.  We use rate M_OSF filter memory to get fine timing
       resolution. */

    s = round(rx_timing) + M_OSF;
    for(c=0; c<NC+1; c++) {
	rx_symbols[c].real = 0.0;
	rx_symbols[c].imag = 0.0;
	for(k=s,j=0; k<s+NFILTER; k++,j++)
	    rx_symbols[c] = cadd(rx_symbols[c], fcmult(gt_alpha5_root[j], rx_baseband_mem_timing[c][k]));
    }
	
    return rx_timing;
}

/*---------------------------------------------------------------------------*\
                                                       
  FUNCTION....: qpsk_to_bits()	     
  AUTHOR......: David Rowe			      
  DATE CREATED: 24/4/2012

  Convert DQPSK symbols back to an array of bits, extracts sync bit
  from DBPSK pilot, and also uses pilot to estimate fine frequency
  error.

\*---------------------------------------------------------------------------*/

float qpsk_to_bits(int rx_bits[], int *sync_bit, COMP phase_difference[], COMP prev_rx_symbols[], COMP rx_symbols[])
{
    int   c;
    COMP  pi_on_4;
    COMP  d;
    int   msb=0, lsb=0;
    float ferr;

    pi_on_4.real = cos(PI/4.0);
    pi_on_4.imag = sin(PI/4.0);

    /* Extra 45 degree clockwise lets us use real and imag axis as
       decision boundaries */

    for(c=0; c<NC; c++)
	phase_difference[c] = cmult(cmult(rx_symbols[c], cconj(prev_rx_symbols[c])), pi_on_4);
				    
    /* map (Nc,1) DQPSK symbols back into an (1,Nc*Nb) array of bits */

    for (c=0; c<NC; c++) {
      d = phase_difference[c];
      if ((d.real >= 0) && (d.imag >= 0)) {
         msb = 0; lsb = 0;
      }
      if ((d.real < 0) && (d.imag >= 0)) {
         msb = 0; lsb = 1;
      }
#ifdef OLD_QPSK_SYMBOLS
      if ((d.real < 0) && (d.imag < 0)) {
         msb = 1; lsb = 0;
      }
      if ((d.real >= 0) && (d.imag < 0)) {
         msb = 1; lsb = 1;
      }
#else
      if ((d.real < 0) && (d.imag < 0)) {
         msb = 1; lsb = 1;
      }
      if ((d.real >= 0) && (d.imag < 0)) {
         msb = 1; lsb = 0;
      }
#endif
      rx_bits[2*c] = msb;
      rx_bits[2*c+1] = lsb;
    }
 
    /* Extract DBPSK encoded Sync bit and fine freq offset estimate */

    phase_difference[NC] = cmult(rx_symbols[NC], cconj(prev_rx_symbols[NC]));
    if (phase_difference[NC].real < 0) {
      *sync_bit = 1;
      ferr = phase_difference[NC].imag;
    }
    else {
      *sync_bit = 0;
      ferr = -phase_difference[NC].imag;
    }

    return ferr;
}
 
/*---------------------------------------------------------------------------*\
                                                       
  FUNCTION....: fdmdv_put_test_bits()	     
  AUTHOR......: David Rowe			      
  DATE CREATED: 24/4/2012

  Accepts nbits from rx and attempts to sync with test_bits sequence.
  If sync OK measures bit errors.

\*---------------------------------------------------------------------------*/

void fdmdv_put_test_bits(struct FDMDV *f, int *sync, int *bit_errors, int *ntest_bits, int rx_bits[])
{
    int   i,j;
    float ber;

    /* Append to our memory */

    for(i=0,j=FDMDV_BITS_PER_FRAME; i<NTEST_BITS-FDMDV_BITS_PER_FRAME; i++,j++)
	f->rx_test_bits_mem[i] = f->rx_test_bits_mem[j];
    for(i=NTEST_BITS-FDMDV_BITS_PER_FRAME,j=0; i<NTEST_BITS; i++,j++)
	f->rx_test_bits_mem[i] = rx_bits[j];
    
    /* see how many bit errors we get when checked against test sequence */
       
    *bit_errors = 0;
    for(i=0; i<NTEST_BITS; i++) {
	*bit_errors += test_bits[i] ^ f->rx_test_bits_mem[i];
	//printf("%d %d %d %d\n", i, test_bits[i], f->rx_test_bits_mem[i], test_bits[i] ^ f->rx_test_bits_mem[i]);
    }

    /* if less than a thresh we are aligned and in sync with test sequence */

    ber = (float)*bit_errors/NTEST_BITS;
  
    *sync = 0;
    if (ber < 0.2)
	*sync = 1;
   
    *ntest_bits = NTEST_BITS;
    
}

/*---------------------------------------------------------------------------*\
                                                       
  FUNCTION....: freq_state(()	     
  AUTHOR......: David Rowe			      
  DATE CREATED: 24/4/2012

  Freq offset state machine.  Moves between coarse and fine states
  based on BPSK pilot sequence.  Freq offset estimator occasionally
  makes mistakes when used continuously.  So we use it until we have
  acquired the BPSK pilot, then switch to a more robust "fine"
  tracking algorithm.  If we lose sync we switch back to coarse mode
  for fast-requisition of large frequency offsets.

\*---------------------------------------------------------------------------*/

int freq_state(int sync_bit, int *state)
{
    int next_state, coarse_fine;

    /* acquire state, look for 6 symbol 010101 sequence from sync bit */

    next_state = *state;
    switch(*state) {
    case 0:
	if (sync_bit == 0)
	    next_state = 1;
	break;
    case 1:
	if (sync_bit == 1)
	    next_state = 2;
	else 
	    next_state = 0;
	break;
    case 2:
	if (sync_bit == 0)
	    next_state = 3;
	else 
	    next_state = 0;
	break;
    case 3:
	if (sync_bit == 1)
	    next_state = 4;
	else 
	    next_state = 0;
	break;
    case 4:
	if (sync_bit == 0)
	    next_state = 5;
	else 
	    next_state = 0;
	break;
    case 5:
	if (sync_bit == 1)
	    next_state = 6;
	else 
	    next_state = 0;
	break;
	
	/* states 6 and above are track mode, make sure we keep
	   getting 0101 sync bit sequence */

    case 6:
	if (sync_bit == 0)
	    next_state = 7;
	else 
	    next_state = 0;

	break;
    case 7:
	if (sync_bit == 1)
	    next_state = 6;
	else 
	    next_state = 0;
	break;
    }

    *state = next_state;
    if (*state >= 6)
	coarse_fine = FINE;
    else
	coarse_fine = COARSE;
 
    return coarse_fine;
}

/*---------------------------------------------------------------------------*\
                                                       
  FUNCTION....: fdmdv_demod()	     
  AUTHOR......: David Rowe			      
  DATE CREATED: 26/4/2012

  FDMDV demodulator, take an array of FDMDV_SAMPLES_PER_FRAME
  modulated symbols, returns an array of FDMDV_BITS_PER_FRAME bits,
  plus the sync bit.  

  The number of input samples nin will normally be M_OSF ==
  FDMDV_SAMPLES_PER_FRAME.  However to adjust for differences in
  transmit and receive sample clocks nin will occasionally be M_OSF-M_OSF/P_OSF,
  or M_OSF+M_OSF/P_OSF.

\*---------------------------------------------------------------------------*/

void fdmdv_demod(struct FDMDV *fdmdv, int rx_bits[], int *sync_bit, float rx_fdm[], int *nin)
{
    float         foff_coarse, foff_fine;
    COMP          rx_fdm_fcorr[M_OSF+M_OSF/P_OSF];
    COMP          rx_baseband[NC+1][M_OSF+M_OSF/P_OSF];
    COMP          rx_filt[NC+1][P_OSF+1];
    COMP          rx_symbols[NC+1];
    float         env[NT*P_OSF];
 
    /* freq offset estimation and correction */
   
    foff_coarse = rx_est_freq_offset(fdmdv, rx_fdm, *nin);
    
    if (fdmdv->coarse_fine == COARSE)
	fdmdv->foff = foff_coarse;
    freq_shift(rx_fdm_fcorr, rx_fdm, fdmdv->foff, &fdmdv->foff_rect, &fdmdv->foff_phase_rect, *nin);
	
    /* baseband processing */

    fdm_downconvert(rx_baseband, rx_fdm_fcorr, fdmdv->phase_rx, fdmdv->freq, *nin);
    //XXX:split rx_filter_memory!
    //rx_filter(rx_filt, rx_baseband, fdmdv->rx_filter_memory, *nin);
    fdmdv->rx_timing = rx_est_timing(rx_symbols, rx_filt, rx_baseband, fdmdv->rx_filter_mem_timing, env, fdmdv->rx_baseband_mem_timing, *nin);	 
    
    /* adjust number of input samples to keep timing within bounds */

    *nin = M_OSF;

    if (fdmdv->rx_timing > 2*M_OSF/P_OSF)
       *nin += M_OSF/P_OSF;
    
    if (fdmdv->rx_timing < 0)
       *nin -= M_OSF/P_OSF;
    
    foff_fine = qpsk_to_bits(rx_bits, sync_bit, fdmdv->phase_difference, fdmdv->prev_rx_symbols, rx_symbols);
    memcpy(fdmdv->prev_rx_symbols, rx_symbols, sizeof(COMP)*(NC+1));

    /* freq offset estimation state machine */

    fdmdv->coarse_fine = freq_state(*sync_bit, &fdmdv->fest_state);
    fdmdv->foff  -= TRACK_COEFF*foff_fine;
}

/*---------------------------------------------------------------------------*\
                                                       
  FUNCTION....: fdmdv_get_demod_stats()	     
  AUTHOR......: David Rowe			      
  DATE CREATED: 1 May 2012

  Fills stats structure with a bunch of demod information.

\*---------------------------------------------------------------------------*/

void fdmdv_get_demod_stats(struct FDMDV *fdmdv, struct FDMDV_STATS *fdmdv_stats)
{
    int   c;
    COMP  pi_on_4;

    pi_on_4.real = cos(PI/4.0);
    pi_on_4.imag = sin(PI/4.0);

    fdmdv_stats->snr = 0.0; /* TODO - implement SNR estimation */
    fdmdv_stats->fest_coarse_fine = fdmdv->coarse_fine;
    fdmdv_stats->foff = fdmdv->foff;
    fdmdv_stats->rx_timing = fdmdv->rx_timing;
    fdmdv_stats->clock_offset = 0.0; /* TODO - implement clock offset estimation */

    assert((NC+1) == FDMDV_NSYM);

    for(c=0; c<NC; c++) {
	fdmdv_stats->rx_symbols[c] = fdmdv->phase_difference[c];
    }
    
    /* place pilots somewhere convenient on scatter diagram */

    fdmdv_stats->rx_symbols[NC] = cmult(fdmdv->phase_difference[NC], pi_on_4);

}
#endif
/*---------------------------------------------------------------------------*\
                                                       
  FUNCTION....: fdmdv_8_to_48()	     
  AUTHOR......: David Rowe			      
  DATE CREATED: 9 May 2012

  Changes the sample rate of a signal from 8 to 48 kHz.  Experience
  with PC based modems has shown that PC sound cards have a more
  accurate sample clock when set for 48 kHz than 8 kHz.

  n is the number of samples at the 8 kHz rate, there are FDMDV_OS*n samples
  at the 48 kHz rate.  A memory of FDMDV_OS_TAPS/FDMDV_OS samples is reqd for
  in8k[] (see t48_8.c unit test as example).

  This is a classic polyphase upsampler.  We take the 8 kHz samples
  and insert (FDMDV_OS-1) zeroes between each sample, then
  FDMDV_OS_TAPS FIR low pass filter the signal at 4kHz.  As most of
  the input samples are zeroes, we only need to multiply non-zero
  input samples by filter coefficients.  The zero insertion and
  filtering are combined in the code below and I'm too lazy to explain
  it further right now....

\*---------------------------------------------------------------------------*/

void fdmdv_8_to_48(float out48k[], float in8k[], int n)
{
    int i,j,k,l;

    for(i=0; i<n; i++) {
	for(j=0; j<FDMDV_OS; j++) {
	    out48k[i*FDMDV_OS+j] = 0.0;
	    for(k=0,l=0; k<FDMDV_OS_TAPS; k+=FDMDV_OS,l++)
		out48k[i*FDMDV_OS+j] += fdmdv_os_filter[k+j]*in8k[i-l];
	    out48k[i*FDMDV_OS+j] *= FDMDV_OS;
	    
	}
    }
}

/*---------------------------------------------------------------------------*\
                                                       
  FUNCTION....: fdmdv_48_to_8()	     
  AUTHOR......: David Rowe			      
  DATE CREATED: 9 May 2012

  Changes the sample rate of a signal from 48 to 8 kHz.
 
  n is the number of samples at the 8 kHz rate, there are FDMDV_OS*n
  samples at the 48 kHz rate.  As above however a memory of
  FDMDV_OS_TAPS samples is reqd for in48k[] (see t48_8.c unit test as example).

  Low pass filter the 48 kHz signal at 4 kHz using the same filter as
  the upsampler, then just output every FDMDV_OS-th filtered sample.

\*---------------------------------------------------------------------------*/

void fdmdv_48_to_8(float out8k[], float in48k[], int n)
{
    int i,j;

    for(i=0; i<n; i++) {
	out8k[i] = 0.0;
	for(j=0; j<FDMDV_OS_TAPS; j++)
	    out8k[i] += fdmdv_os_filter[j]*in48k[i*FDMDV_OS-j];
    }
}

