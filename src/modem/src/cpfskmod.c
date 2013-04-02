/*
 * Copyright (c) 2007, 2008, 2009, 2010, 2011, 2012, 2013 Joseph Gaeddert
 *
 * This file is part of liquid.
 *
 * liquid is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * liquid is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with liquid.  If not, see <http://www.gnu.org/licenses/>.
 */

//
// continuous phase frequency-shift keying modulator
//

#include <stdlib.h>
#include <stdio.h>
#include <math.h>

#include "liquid.internal.h"

// 
// internal methods
//

// design transmit filter
void cpfskmod_firdes(unsigned int _k,
                     unsigned int _m,
                     float        _beta,
                     int          _type,
                     float *      _h,
                     unsigned int _h_len);

// cpfskmod
struct cpfskmod_s {
    // common
    unsigned int bps;           // bits per symbol
    unsigned int k;             // samples per symbol
    unsigned int m;             // filter delay (symbols)
    float        beta;          // filter bandwidth parameter
    float        h;             // modulation index
    int          type;          // filter type (e.g. LIQUID_CPFSK_SQUARE)

    // constellation size
    unsigned int M;

    // transmit filter delay (symbols)
    // TODO: coordinate this value with 'm'
    unsigned int tx_delay;

    // pulse-shaping filter
    float * ht;                 // filter coefficients
    unsigned int ht_len;        // filter length
    interp_rrrf  interp;        // interpolator

    // phase integrator
    float * phase_interp;       // phase interpolation buffer
    iirfilt_rrrf integrator;    // integrator
};

// create cpfskmod object (frequency modulator)
//  _bps    :   bits per symbol, _bps > 0
//  _h      :   modulation index, _h > 0
//  _k      :   samples/symbol, _k > 1, _k even
//  _m      :   filter delay (symbols), _m > 0
//  _beta   :   filter bandwidth parameter, _beta > 0
//  _type   :   filter type (e.g. LIQUID_CPFSK_SQUARE)
cpfskmod cpfskmod_create(unsigned int _bps,
                         float        _h,
                         unsigned int _k,
                         unsigned int _m,
                         float        _beta,
                         int          _type)
{
    // validate input
    if (_bps == 0) {
        fprintf(stderr,"error: cpfskmod_create(), bits/symbol must be greater than 0\n");
        exit(1);
    } else if (_k < 2 || (_k%2)) {
        fprintf(stderr,"error: cpfskmod_create(), samples/symbol must be greater than 2 and even\n");
        exit(1);
    } else if (_m == 0) {
        fprintf(stderr,"error: cpfskmod_create(), filter delay must be greater than 0\n");
        exit(1);
    } else if (_beta <= 0.0f || _beta > 1.0f) {
        fprintf(stderr,"error: cpfskmod_create(), filter roll-off must be in (0,1]\n");
        exit(1);
    } else if (_h <= 0.0f) {
        fprintf(stderr,"error: cpfskmod_create(), modulation index must be greater than 0\n");
        exit(1);
    }

    // create main object memory
    cpfskmod q = (cpfskmod) malloc(sizeof(struct cpfskmod_s));

    // set basic internal properties
    q->bps  = _bps;     // bits per symbol
    q->h    = _h;       // modulation index
    q->k    = _k;       // samples per symbol
    q->m    = _m;       // filter delay (symbols)
    q->beta = _beta;    // filter roll-off factor (only for certain filters)
    q->type = _type;    // filter type

    // derived values
    q->M = 1 << q->bps; // constellation size

    // create object depending upon input type
    float b[2] = {0.5f,  0.5f}; // integrator feed-forward coefficients
    float a[2] = {1.0f, -1.0f}; // integrator feed-back coefficients
    q->ht_len = 0;
    q->ht = NULL;
    unsigned int i;
    switch(q->type) {
    case LIQUID_CPFSK_SQUARE:
        q->ht_len = q->k;
        // modify integrator
        b[0] = 0.0f;
        b[1] = 1.0f;
        break;
    case LIQUID_CPFSK_RCOS_FULL:
        q->ht_len = q->k;
        break;
    case LIQUID_CPFSK_RCOS_PARTIAL:
        // TODO: adjust reponse based on 'm'
        q->ht_len = 3*q->k;
        break;
    case LIQUID_CPFSK_GMSK:
        q->ht_len = 2*(q->k)*(q->m) + (q->k) + 1;
        break;
    default:
        fprintf(stderr,"error: cpfskmodem_create(), invalid filter type '%d'\n", q->type);
        exit(1);
    }

    // create pulse-shaping filter and scale by modulation index
    q->ht = (float*) malloc(q->ht_len *sizeof(float));
    cpfskmod_firdes(q->k, q->m, q->beta, q->type, q->ht, q->ht_len);
    for (i=0; i<q->ht_len; i++)
        q->ht[i] *= M_PI * q->h;
    q->interp = interp_rrrf_create(q->k, q->ht, q->ht_len);

    // create phase integrator
    q->phase_interp = (float*) malloc(q->k*sizeof(float));
    q->integrator = iirfilt_rrrf_create(b,2,a,2);

    // reset modem object
    cpfskmod_reset(q);

    return q;
}

// destroy cpfskmod object
void cpfskmod_destroy(cpfskmod _q)
{
    // destroy pulse-shaping filter/interpolator
    free(_q->ht);
    free(_q->phase_interp);
    interp_rrrf_destroy(_q->interp);

    // destroy phase integrator
    iirfilt_rrrf_destroy(_q->integrator);

    // free main object memory
    free(_q);
}

// print cpfskmod object internals
void cpfskmod_print(cpfskmod _q)
{
    printf("cpfskmod : continuous-phase frequency-shift keying modem\n");
    printf("    bits/symbol     :   %u\n", _q->bps);
    printf("    modulation index:   %-6.3f\n", _q->h);
    printf("    samples/symbol  :   %u\n", _q->k);
    printf("    filter delay    :   %u symbols\n", _q->m);
    printf("    filter roll-off :   %-6.3f\n", _q->beta);
    printf("    filter type     :   ");
    switch(_q->type) {
    case LIQUID_CPFSK_SQUARE:       printf("square\n");         break;
    case LIQUID_CPFSK_RCOS_FULL:    printf("rcos (full)\n");    break;
    case LIQUID_CPFSK_RCOS_PARTIAL: printf("rcos (partial)\n"); break;
    case LIQUID_CPFSK_GMSK:         printf("gmsk\n");           break;
    default:                        printf("unknown\n");        break;
    }
    printf("    filter          :\n");
    // print filter coefficients
    unsigned int i;
    for (i=0; i<_q->ht_len; i++)
        printf("        h(%3u) = %12.8f;\n", i+1, _q->ht[i]);
}

// reset state
void cpfskmod_reset(cpfskmod _q)
{
    // reset interpolator
    interp_rrrf_clear(_q->interp);

    // reset phase integrator
    iirfilt_rrrf_clear(_q->integrator);
}

// modulate sample
//  _q      :   frequency modulator object
//  _s      :   input symbol
//  _y      :   output sample array [size: _k x 1]
void cpfskmod_modulate(cpfskmod        _q,
                       unsigned int    _s,
                       float complex * _y)
{
    // run interpolator
    float v = 2.0f*_s - (float)(_q->M) + 1.0f;
    interp_rrrf_execute(_q->interp, v, _q->phase_interp);

    // integrate phase state
    unsigned int i;
    float theta;
    for (i=0; i<_q->k; i++) {
        // push phase through integrator
        iirfilt_rrrf_execute(_q->integrator, _q->phase_interp[i], &theta);

        // compute output
        _y[i] = liquid_cexpjf(theta);
    }
}

// 
// internal methods
//

// design transmit filter
void cpfskmod_firdes(unsigned int _k,
                     unsigned int _m,
                     float        _beta,
                     int          _type,
                     float *      _ht,
                     unsigned int _ht_len)
{
    unsigned int i;
    // create filter based on specified type
    switch(_type) {
    case LIQUID_CPFSK_SQUARE:
        // square pulse
        if (_ht_len != _k) {
            fprintf(stderr,"error: cpfskmodem_firdes(), invalid filter length (square)\n");
            exit(1);
        }
        for (i=0; i<_ht_len; i++)
            _ht[i] = 1.0f;
        break;
    case LIQUID_CPFSK_RCOS_FULL:
        // full-response raised-cosine pulse
        if (_ht_len != _k) {
            fprintf(stderr,"error: cpfskmodem_firdes(), invalid filter length (rcos)\n");
            exit(1);
        }
        for (i=0; i<_ht_len; i++)
            _ht[i] = 1.0f - cosf(2.0f*M_PI*i/(float)_ht_len);
        break;
    case LIQUID_CPFSK_RCOS_PARTIAL:
        // full-response raised-cosine pulse
        if (_ht_len != 3*_k) {
            fprintf(stderr,"error: cpfskmodem_firdes(), invalid filter length (rcos)\n");
            exit(1);
        }
        // initialize with zeros
        for (i=0; i<_ht_len; i++)
            _ht[i] = 0.0f;
        // adding raised-cosine pulse with half-symbol delay
        for (i=0; i<2*_k; i++)
            _ht[i+_k/2] = 1.0f - cosf(2.0f*M_PI*i/(float)(2*_k));
        break;
    case LIQUID_CPFSK_GMSK:
        // Gauss minimum-shift keying pulse
        if (_ht_len != 2*_k*_m + _k + 1) {
            fprintf(stderr,"error: cpfskmodem_firdes(), invalid filter length (gmsk)\n");
            exit(1);
        }
        // initialize with zeros
        for (i=0; i<_ht_len; i++)
            _ht[i] = 0.0f;
        // adding Gauss pulse with half-symbol delay
        liquid_firdes_gmsktx(_k,_m,_beta,0.0f,&_ht[_k/2]);
        break;
    default:
        fprintf(stderr,"error: cpfskmodem_firdes(), invalid filter type '%d'\n", _type);
        exit(1);
    }

    // normalize pulse area to unity
    float ht_sum = 0.0f;
    for (i=0; i<_ht_len; i++)
        ht_sum += _ht[i];
    for (i=0; i<_ht_len; i++)
        _ht[i] *= 1.0f / ht_sum;
}

