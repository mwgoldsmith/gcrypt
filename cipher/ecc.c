/* ecc.c  -  Elliptic Curve Cryptography
 * Copyright (C) 2007, 2008, 2010, 2011 Free Software Foundation, Inc.
 * Copyright (C) 2013 g10 Code GmbH
 *
 * This file is part of Libgcrypt.
 *
 * Libgcrypt is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation; either version 2.1 of
 * the License, or (at your option) any later version.
 *
 * Libgcrypt is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

/* This code is originally based on the Patch 0.1.6 for the gnupg
   1.4.x branch as retrieved on 2007-03-21 from
   http://www.calcurco.cat/eccGnuPG/src/gnupg-1.4.6-ecc0.2.0beta1.diff.bz2
   The original authors are:
     Written by
      Sergi Blanch i Torne <d4372211 at alumnes.eup.udl.es>,
      Ramiro Moreno Chiral <ramiro at eup.udl.es>
     Maintainers
      Sergi Blanch i Torne
      Ramiro Moreno Chiral
      Mikael Mylnikov (mmr)
  For use in Libgcrypt the code has been heavily modified and cleaned
  up. In fact there is not much left of the orginally code except for
  some variable names and the text book implementaion of the sign and
  verification algorithms.  The arithmetic functions have entirely
  been rewritten and moved to mpi/ec.c.

  ECDH encrypt and decrypt code written by Andrey Jivsov,
*/


/* TODO:

  - If we support point compression we need to uncompress before
    computing the keygrip

  - In mpi/ec.c we use mpi_powm for x^2 mod p: Either implement a
    special case in mpi_powm or check whether mpi_mulm is faster.

*/


#include <config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "g10lib.h"
#include "mpi.h"
#include "cipher.h"
#include "context.h"
#include "ec-context.h"
#include "pubkey-internal.h"
#include "ecc-common.h"


/* Registered progress function and its callback value. */
static void (*progress_cb) (void *, const char*, int, int, int);
static void *progress_cb_data;


#define point_init(a)  _gcry_mpi_point_init ((a))
#define point_free(a)  _gcry_mpi_point_free_parts ((a))


/* Local prototypes. */
static void test_keys (ECC_secret_key * sk, unsigned int nbits);
static int check_secret_key (ECC_secret_key * sk);
static gpg_err_code_t sign_ecdsa (gcry_mpi_t input, ECC_secret_key *skey,
                                  gcry_mpi_t r, gcry_mpi_t s,
                                  int flags, int hashalgo);
static gpg_err_code_t verify_ecdsa (gcry_mpi_t input, ECC_public_key *pkey,
                                    gcry_mpi_t r, gcry_mpi_t s);

static gcry_mpi_t gen_y_2 (gcry_mpi_t x, elliptic_curve_t * base);




void
_gcry_register_pk_ecc_progress (void (*cb) (void *, const char *,
                                            int, int, int),
                                void *cb_data)
{
  progress_cb = cb;
  progress_cb_data = cb_data;
}

/* static void */
/* progress (int c) */
/* { */
/*   if (progress_cb) */
/*     progress_cb (progress_cb_data, "pk_ecc", c, 0, 0); */
/* } */




/*
 * Solve the right side of the Weierstrass equation.
 */
static gcry_mpi_t
gen_y_2 (gcry_mpi_t x, elliptic_curve_t *base)
{
  gcry_mpi_t three, x_3, axb, y;

  three = mpi_alloc_set_ui (3);
  x_3 = mpi_new (0);
  axb = mpi_new (0);
  y   = mpi_new (0);

  mpi_powm (x_3, x, three, base->p);
  mpi_mulm (axb, base->a, x, base->p);
  mpi_addm (axb, axb, base->b, base->p);
  mpi_addm (y, x_3, axb, base->p);

  mpi_free (x_3);
  mpi_free (axb);
  mpi_free (three);
  return y; /* The quadratic value of the coordinate if it exist. */
}


/* Standard version of the key generation.  */
static gpg_err_code_t
nist_generate_key (ECC_secret_key *sk, elliptic_curve_t *E, mpi_ec_t ctx,
                   gcry_random_level_t random_level, unsigned int nbits)
{
  mpi_point_struct Q;

  point_init (&Q);

  /* Generate a secret.  */
  sk->d = _gcry_dsa_gen_k (E->n, random_level);

  /* Compute Q.  */
  _gcry_mpi_ec_mul_point (&Q, sk->d, &E->G, ctx);

  /* Copy the stuff to the key structures. */
  sk->E.model = E->model;
  sk->E.dialect = E->dialect;
  sk->E.p = mpi_copy (E->p);
  sk->E.a = mpi_copy (E->a);
  sk->E.b = mpi_copy (E->b);
  point_init (&sk->E.G);
  point_set (&sk->E.G, &E->G);
  sk->E.n = mpi_copy (E->n);
  point_init (&sk->Q);

  /* We want the Q=(x,y) be a "compliant key" in terms of the
   * http://tools.ietf.org/html/draft-jivsov-ecc-compact, which simply
   * means that we choose either Q=(x,y) or -Q=(x,p-y) such that we
   * end up with the min(y,p-y) as the y coordinate.  Such a public
   * key allows the most efficient compression: y can simply be
   * dropped because we know that it's a minimum of the two
   * possibilities without any loss of security.  */
  {
    gcry_mpi_t x, y, p_y;
    const unsigned int pbits = mpi_get_nbits (E->p);

    x = mpi_new (pbits);
    y = mpi_new (pbits);
    p_y = mpi_new (pbits);

    if (_gcry_mpi_ec_get_affine (x, y, &Q, ctx))
      log_fatal ("ecgen: Failed to get affine coordinates for %s\n", "Q");

    mpi_sub (p_y, E->p, y);	/* p_y = p - y */

    if (mpi_cmp (p_y, y) < 0)   /* p - y < p */
      {
        /* We need to end up with -Q; this assures that new Q's y is
           the smallest one */
        mpi_sub (sk->d, E->n, sk->d);   /* d = order - d */
        gcry_mpi_point_snatch_set (&sk->Q, x, p_y, mpi_alloc_set_ui (1));

      if (DBG_CIPHER)
        log_debug ("ecgen converted Q to a compliant point\n");
      }
    else /* p - y >= p */
      {
        /* No change is needed exactly 50% of the time: just copy. */
        point_set (&sk->Q, &Q);
        if (DBG_CIPHER)
          log_debug ("ecgen didn't need to convert Q to a compliant point\n");

        mpi_free (p_y);
        mpi_free (x);
      }
    mpi_free (y);
  }

  /* Now we can test our keys (this should never fail!).  */
  test_keys (sk, nbits - 64);

  return 0;
}


/*
 * To verify correct skey it use a random information.
 * First, encrypt and decrypt this dummy value,
 * test if the information is recuperated.
 * Second, test with the sign and verify functions.
 */
static void
test_keys (ECC_secret_key *sk, unsigned int nbits)
{
  ECC_public_key pk;
  gcry_mpi_t test = mpi_new (nbits);
  mpi_point_struct R_;
  gcry_mpi_t c = mpi_new (nbits);
  gcry_mpi_t out = mpi_new (nbits);
  gcry_mpi_t r = mpi_new (nbits);
  gcry_mpi_t s = mpi_new (nbits);

  if (DBG_CIPHER)
    log_debug ("Testing key.\n");

  point_init (&R_);

  pk.E = _gcry_ecc_curve_copy (sk->E);
  point_init (&pk.Q);
  point_set (&pk.Q, &sk->Q);

  gcry_mpi_randomize (test, nbits, GCRY_WEAK_RANDOM);

  if (sign_ecdsa (test, sk, r, s, 0, 0) )
    log_fatal ("ECDSA operation: sign failed\n");

  if (verify_ecdsa (test, &pk, r, s))
    {
      log_fatal ("ECDSA operation: sign, verify failed\n");
    }

  if (DBG_CIPHER)
    log_debug ("ECDSA operation: sign, verify ok.\n");

  point_free (&pk.Q);
  _gcry_ecc_curve_free (&pk.E);

  point_free (&R_);
  mpi_free (s);
  mpi_free (r);
  mpi_free (out);
  mpi_free (c);
  mpi_free (test);
}


/*
 * To check the validity of the value, recalculate the correspondence
 * between the public value and the secret one.
 */
static int
check_secret_key (ECC_secret_key * sk)
{
  int rc = 1;
  mpi_point_struct Q;
  gcry_mpi_t y_2, y2;
  gcry_mpi_t x1, x2;
  mpi_ec_t ctx = NULL;

  point_init (&Q);

  /* ?primarity test of 'p' */
  /*  (...) //!! */
  /* G in E(F_p) */
  y_2 = gen_y_2 (sk->E.G.x, &sk->E);   /*  y^2=x^3+a*x+b */
  y2 = mpi_alloc (0);
  x1 = mpi_alloc (0);
  x2 = mpi_alloc (0);
  mpi_mulm (y2, sk->E.G.y, sk->E.G.y, sk->E.p);      /*  y^2=y*y */
  if (mpi_cmp (y_2, y2))
    {
      if (DBG_CIPHER)
        log_debug ("Bad check: Point 'G' does not belong to curve 'E'!\n");
      goto leave;
    }
  /* G != PaI */
  if (!mpi_cmp_ui (sk->E.G.z, 0))
    {
      if (DBG_CIPHER)
        log_debug ("Bad check: 'G' cannot be Point at Infinity!\n");
      goto leave;
    }

  ctx = _gcry_mpi_ec_p_internal_new (sk->E.model, sk->E.dialect,
                                     sk->E.p, sk->E.a, sk->E.b);

  _gcry_mpi_ec_mul_point (&Q, sk->E.n, &sk->E.G, ctx);
  if (mpi_cmp_ui (Q.z, 0))
    {
      if (DBG_CIPHER)
        log_debug ("check_secret_key: E is not a curve of order n\n");
      goto leave;
    }
  /* pubkey cannot be PaI */
  if (!mpi_cmp_ui (sk->Q.z, 0))
    {
      if (DBG_CIPHER)
        log_debug ("Bad check: Q can not be a Point at Infinity!\n");
      goto leave;
    }
  /* pubkey = [d]G over E */
  _gcry_mpi_ec_mul_point (&Q, sk->d, &sk->E.G, ctx);

  if (_gcry_mpi_ec_get_affine (x1, y_2, &Q, ctx))
    {
      if (DBG_CIPHER)
        log_debug ("Bad check: Q can not be a Point at Infinity!\n");
      goto leave;
    }

  /* Fast path for loaded secret keys - Q is already in affine coordinates */
  if (!mpi_cmp_ui (sk->Q.z, 1))
    {
      if (mpi_cmp (x1, sk->Q.x) || mpi_cmp (y_2, sk->Q.y))
        {
          if (DBG_CIPHER)
            log_debug
              ("Bad check: There is NO correspondence between 'd' and 'Q'!\n");
          goto leave;
        }
    }
  else
    {
      if (_gcry_mpi_ec_get_affine (x2, y2, &sk->Q, ctx))
        {
          if (DBG_CIPHER)
            log_debug ("Bad check: Q can not be a Point at Infinity!\n");
          goto leave;
        }

      if (mpi_cmp (x1, x2) || mpi_cmp (y_2, y2))
        {
          if (DBG_CIPHER)
            log_debug
              ("Bad check: There is NO correspondence between 'd' and 'Q'!\n");
          goto leave;
        }
    }
  rc = 0; /* Okay.  */

 leave:
  _gcry_mpi_ec_free (ctx);
  mpi_free (x2);
  mpi_free (x1);
  mpi_free (y2);
  mpi_free (y_2);
  point_free (&Q);
  return rc;
}


/* Compute an ECDSA signature.
 * Return the signature struct (r,s) from the message hash.  The caller
 * must have allocated R and S.
 */
static gpg_err_code_t
sign_ecdsa (gcry_mpi_t input, ECC_secret_key *skey, gcry_mpi_t r, gcry_mpi_t s,
            int flags, int hashalgo)
{
  gpg_err_code_t err = 0;
  int extraloops = 0;
  gcry_mpi_t k, dr, sum, k_1, x;
  mpi_point_struct I;
  gcry_mpi_t hash;
  const void *abuf;
  unsigned int abits, qbits;
  mpi_ec_t ctx;

  if (DBG_CIPHER)
    log_mpidump ("ecdsa sign hash  ", input );

  qbits = mpi_get_nbits (skey->E.n);

  /* Convert the INPUT into an MPI if needed.  */
  if (mpi_is_opaque (input))
    {
      abuf = gcry_mpi_get_opaque (input, &abits);
      err = gpg_err_code (gcry_mpi_scan (&hash, GCRYMPI_FMT_USG,
                                         abuf, (abits+7)/8, NULL));
      if (err)
        return err;
      if (abits > qbits)
        gcry_mpi_rshift (hash, hash, abits - qbits);
    }
  else
    hash = input;


  k = NULL;
  dr = mpi_alloc (0);
  sum = mpi_alloc (0);
  k_1 = mpi_alloc (0);
  x = mpi_alloc (0);
  point_init (&I);

  mpi_set_ui (s, 0);
  mpi_set_ui (r, 0);

  ctx = _gcry_mpi_ec_p_internal_new (skey->E.model, skey->E.dialect,
                                     skey->E.p, skey->E.a, skey->E.b);

  while (!mpi_cmp_ui (s, 0)) /* s == 0 */
    {
      while (!mpi_cmp_ui (r, 0)) /* r == 0 */
        {
          /* Note, that we are guaranteed to enter this loop at least
             once because r has been intialized to 0.  We can't use a
             do_while because we want to keep the value of R even if S
             has to be recomputed.  */

          mpi_free (k);
          k = NULL;
          if ((flags & PUBKEY_FLAG_RFC6979) && hashalgo)
            {
              /* Use Pornin's method for deterministic DSA.  If this
                 flag is set, it is expected that HASH is an opaque
                 MPI with the to be signed hash.  That hash is also
                 used as h1 from 3.2.a.  */
              if (!mpi_is_opaque (input))
                {
                  err = GPG_ERR_CONFLICT;
                  goto leave;
                }

              abuf = gcry_mpi_get_opaque (input, &abits);
              err = _gcry_dsa_gen_rfc6979_k (&k, skey->E.n, skey->d,
                                             abuf, (abits+7)/8,
                                             hashalgo, extraloops);
              if (err)
                goto leave;
              extraloops++;
            }
          else
            k = _gcry_dsa_gen_k (skey->E.n, GCRY_STRONG_RANDOM);

          _gcry_mpi_ec_mul_point (&I, k, &skey->E.G, ctx);
          if (_gcry_mpi_ec_get_affine (x, NULL, &I, ctx))
            {
              if (DBG_CIPHER)
                log_debug ("ecc sign: Failed to get affine coordinates\n");
              err = GPG_ERR_BAD_SIGNATURE;
              goto leave;
            }
          mpi_mod (r, x, skey->E.n);  /* r = x mod n */
        }
      mpi_mulm (dr, skey->d, r, skey->E.n); /* dr = d*r mod n  */
      mpi_addm (sum, hash, dr, skey->E.n);  /* sum = hash + (d*r) mod n  */
      mpi_invm (k_1, k, skey->E.n);         /* k_1 = k^(-1) mod n  */
      mpi_mulm (s, k_1, sum, skey->E.n);    /* s = k^(-1)*(hash+(d*r)) mod n */
    }

  if (DBG_CIPHER)
    {
      log_mpidump ("ecdsa sign result r ", r);
      log_mpidump ("ecdsa sign result s ", s);
    }

 leave:
  _gcry_mpi_ec_free (ctx);
  point_free (&I);
  mpi_free (x);
  mpi_free (k_1);
  mpi_free (sum);
  mpi_free (dr);
  mpi_free (k);

  if (hash != input)
    mpi_free (hash);

  return err;
}


/* Verify an ECDSA signature.
 * Check if R and S verifies INPUT.
 */
static gpg_err_code_t
verify_ecdsa (gcry_mpi_t input, ECC_public_key *pkey,
              gcry_mpi_t r, gcry_mpi_t s)
{
  gpg_err_code_t err = 0;
  gcry_mpi_t h, h1, h2, x;
  mpi_point_struct Q, Q1, Q2;
  mpi_ec_t ctx;

  if( !(mpi_cmp_ui (r, 0) > 0 && mpi_cmp (r, pkey->E.n) < 0) )
    return GPG_ERR_BAD_SIGNATURE; /* Assertion	0 < r < n  failed.  */
  if( !(mpi_cmp_ui (s, 0) > 0 && mpi_cmp (s, pkey->E.n) < 0) )
    return GPG_ERR_BAD_SIGNATURE; /* Assertion	0 < s < n  failed.  */

  h  = mpi_alloc (0);
  h1 = mpi_alloc (0);
  h2 = mpi_alloc (0);
  x = mpi_alloc (0);
  point_init (&Q);
  point_init (&Q1);
  point_init (&Q2);

  ctx = _gcry_mpi_ec_p_internal_new (pkey->E.model, pkey->E.dialect,
                                     pkey->E.p, pkey->E.a, pkey->E.b);

  /* h  = s^(-1) (mod n) */
  mpi_invm (h, s, pkey->E.n);
  /* h1 = hash * s^(-1) (mod n) */
  mpi_mulm (h1, input, h, pkey->E.n);
  /* Q1 = [ hash * s^(-1) ]G  */
  _gcry_mpi_ec_mul_point (&Q1, h1, &pkey->E.G, ctx);
  /* h2 = r * s^(-1) (mod n) */
  mpi_mulm (h2, r, h, pkey->E.n);
  /* Q2 = [ r * s^(-1) ]Q */
  _gcry_mpi_ec_mul_point (&Q2, h2, &pkey->Q, ctx);
  /* Q  = ([hash * s^(-1)]G) + ([r * s^(-1)]Q) */
  _gcry_mpi_ec_add_points (&Q, &Q1, &Q2, ctx);

  if (!mpi_cmp_ui (Q.z, 0))
    {
      if (DBG_CIPHER)
          log_debug ("ecc verify: Rejected\n");
      err = GPG_ERR_BAD_SIGNATURE;
      goto leave;
    }
  if (_gcry_mpi_ec_get_affine (x, NULL, &Q, ctx))
    {
      if (DBG_CIPHER)
        log_debug ("ecc verify: Failed to get affine coordinates\n");
      err = GPG_ERR_BAD_SIGNATURE;
      goto leave;
    }
  mpi_mod (x, x, pkey->E.n); /* x = x mod E_n */
  if (mpi_cmp (x, r))   /* x != r */
    {
      if (DBG_CIPHER)
        {
          log_mpidump ("     x", x);
          log_mpidump ("     r", r);
          log_mpidump ("     s", s);
          log_debug ("ecc verify: Not verified\n");
        }
      err = GPG_ERR_BAD_SIGNATURE;
      goto leave;
    }
  if (DBG_CIPHER)
    log_debug ("ecc verify: Accepted\n");

 leave:
  _gcry_mpi_ec_free (ctx);
  point_free (&Q2);
  point_free (&Q1);
  point_free (&Q);
  mpi_free (x);
  mpi_free (h2);
  mpi_free (h1);
  mpi_free (h);
  return err;
}



static void
reverse_buffer (unsigned char *buffer, unsigned int length)
{
  unsigned int tmp, i;

  for (i=0; i < length/2; i++)
    {
      tmp = buffer[i];
      buffer[i] = buffer[length-1-i];
      buffer[length-1-i] = tmp;
    }
}


/* Encode MPI using the EdDSA scheme.  MINLEN specifies the required
   length of the buffer in bytes.  On success 0 is returned an a
   malloced buffer with the encoded point is stored at R_BUFFER; the
   length of this buffer is stored at R_BUFLEN.  */
static gpg_err_code_t
eddsa_encodempi (gcry_mpi_t mpi, unsigned int minlen,
                 unsigned char **r_buffer, unsigned int *r_buflen)
{
  unsigned char *rawmpi;
  unsigned int rawmpilen;

  rawmpi = _gcry_mpi_get_buffer (mpi, minlen, &rawmpilen, NULL);
  if (!rawmpi)
    return gpg_err_code_from_syserror ();

  *r_buffer = rawmpi;
  *r_buflen = rawmpilen;
  return 0;
}


/* Encode POINT using the EdDSA scheme.  X and Y are scratch variables
   supplied by the caller and CTX is the usual context.  MINLEN is the
   required length in bytes for the result. On success 0 is returned
   an a malloced buffer with the encoded point is stored at R_BUFFER;
   the length of this buffer is stored at R_BUFLEN.  */
static gpg_err_code_t
eddsa_encodepoint (mpi_point_t point, unsigned int minlen, mpi_ec_t ctx,
                   gcry_mpi_t x, gcry_mpi_t y,
                   unsigned char **r_buffer, unsigned int *r_buflen)
{
  unsigned char *rawmpi;
  unsigned int rawmpilen;

  if (_gcry_mpi_ec_get_affine (x, y, point, ctx))
    {
      log_error ("eddsa_encodepoint: Failed to get affine coordinates\n");
      return GPG_ERR_INTERNAL;
    }
  rawmpi = _gcry_mpi_get_buffer (y, minlen, &rawmpilen, NULL);
  if (!rawmpi)
    return gpg_err_code_from_syserror ();
  if (mpi_test_bit (x, 0) && rawmpilen)
    rawmpi[rawmpilen - 1] |= 0x80;  /* Set sign bit.  */

  *r_buffer = rawmpi;
  *r_buflen = rawmpilen;
  return 0;
}


/* Decode the EdDSA style encoded PK and set it into RESULT.  LEN is
   the expected length in bytes of the encoded key and CTX the usual
   curve context.  If R_ENCPK is not NULL, the encoded PK is stored at
   that address; this is a new copy to be release by the caller.  In
   contrast to the supplied PK, this is not an MPI and thus guarnteed
   to be properly padded.  R_ENCPKLEN received the length of that
   encoded key.  */
static gpg_err_code_t
eddsa_decodepoint (gcry_mpi_t pk, unsigned int len, mpi_ec_t ctx,
                   mpi_point_t result,
                   unsigned char **r_encpk, unsigned int *r_encpklen)
{
  unsigned char *rawmpi;
  unsigned int rawmpilen;
  gcry_mpi_t yy, t, x, p1, p2, p3;
  int sign;

  if (mpi_is_opaque (pk))
    {
      const void *buf;

      buf = gcry_mpi_get_opaque (pk, &rawmpilen);
      if (!buf)
        return GPG_ERR_INV_OBJ;
      rawmpilen = (rawmpilen + 7)/8;
      rawmpi = gcry_malloc (rawmpilen? rawmpilen:1);
      if (!rawmpi)
        return gpg_err_code_from_syserror ();
      memcpy (rawmpi, buf, rawmpilen);
      reverse_buffer (rawmpi, rawmpilen);
    }
  else
    {
      rawmpi = _gcry_mpi_get_buffer (pk, len, &rawmpilen, NULL);
      if (!rawmpi)
        return gpg_err_code_from_syserror ();
    }

  if (rawmpilen)
    {
      sign = !!(rawmpi[0] & 0x80);
      rawmpi[0] &= 0x7f;
    }
  else
    sign = 0;
  _gcry_mpi_set_buffer (result->y, rawmpi, rawmpilen, 0);
  if (r_encpk)
    {
      /* Revert to little endian.  */
      if (sign && rawmpilen)
        rawmpi[0] |= 0x80;
      reverse_buffer (rawmpi, rawmpilen);
      *r_encpk = rawmpi;
      if (r_encpklen)
        *r_encpklen = rawmpilen;
    }
  else
    gcry_free (rawmpi);

  /* Now recover X.  */
  /* t = (y^2-1) · ((b*y^2+1)^{p-2} mod p) */
  x = mpi_new (0);
  yy = mpi_new (0);
  mpi_mul (yy, result->y, result->y);
  t = mpi_copy (yy);
  mpi_mul (t, t, ctx->b);
  mpi_add_ui (t, t, 1);
  p2 = mpi_copy (ctx->p);
  mpi_sub_ui (p2, p2, 2);
  mpi_powm (t, t, p2, ctx->p);

  mpi_sub_ui (yy, yy, 1);
  mpi_mul (t, yy, t);

  /* x = t^{(p+3)/8} mod p */
  p3 = mpi_copy (ctx->p);
  mpi_add_ui (p3, p3, 3);
  mpi_fdiv_q (p3, p3, mpi_const (MPI_C_EIGHT));
  mpi_powm (x, t, p3, ctx->p);

  /* (x^2 - t) % p != 0 ? x = (x*(2^{(p-1)/4} mod p)) % p */
  mpi_mul (yy, x, x);
  mpi_subm (yy, yy, t, ctx->p);
  if (mpi_cmp_ui (yy, 0))
    {
      p1 = mpi_copy (ctx->p);
      mpi_sub_ui (p1, p1, 1);
      mpi_fdiv_q (p1, p1, mpi_const (MPI_C_FOUR));
      mpi_powm (yy, mpi_const (MPI_C_TWO), p1, ctx->p);
      mpi_mulm (x, x, yy, ctx->p);
    }
  else
    p1 = NULL;

  /* is_odd(x) ? x = p-x */
  if (mpi_test_bit (x, 0))
    mpi_sub (x, ctx->p, x);

  /* lowbit(x) != highbit(input) ?  x = p-x */
  if (mpi_test_bit (x, 0) != sign)
    mpi_sub (x, ctx->p, x);

  mpi_set (result->x, x);
  mpi_set_ui (result->z, 1);

  gcry_mpi_release (x);
  gcry_mpi_release (yy);
  gcry_mpi_release (t);
  gcry_mpi_release (p3);
  gcry_mpi_release (p2);
  gcry_mpi_release (p1);

  return 0;
}


/* Ed25519 version of the key generation.  */
static gpg_err_code_t
eddsa_generate_key (ECC_secret_key *sk, elliptic_curve_t *E, mpi_ec_t ctx,
                    gcry_random_level_t random_level)
{
  gpg_err_code_t rc;
  int b = 256/8;             /* The only size we currently support.  */
  gcry_mpi_t a, x, y;
  mpi_point_struct Q;
  char *dbuf;
  size_t dlen;
  gcry_buffer_t hvec[1];
  unsigned char *hash_d = NULL;

  point_init (&Q);
  memset (hvec, 0, sizeof hvec);

  a = mpi_snew (0);
  x = mpi_new (0);
  y = mpi_new (0);

  /* Generate a secret.  */
  hash_d = gcry_malloc_secure (2*b);
  if (!hash_d)
    {
      rc = gpg_error_from_syserror ();
      goto leave;
    }
  dlen = b;
  dbuf = gcry_random_bytes_secure (dlen, random_level);

  /* Compute the A value.  */
  hvec[0].data = dbuf;
  hvec[0].len = dlen;
  rc = _gcry_md_hash_buffers (GCRY_MD_SHA512, 0, hash_d, hvec, 1);
  if (rc)
    goto leave;
  sk->d = _gcry_mpi_set_opaque (NULL, dbuf, dlen*8);
  dbuf = NULL;
  reverse_buffer (hash_d, 32);  /* Only the first half of the hash.  */
  hash_d[0] = (hash_d[0] & 0x7f) | 0x40;
  hash_d[31] &= 0xf8;
  _gcry_mpi_set_buffer (a, hash_d, 32, 0);
  gcry_free (hash_d); hash_d = NULL;
  /* log_printmpi ("ecgen         a", a); */

  /* Compute Q.  */
  _gcry_mpi_ec_mul_point (&Q, a, &E->G, ctx);
  if (DBG_CIPHER)
    log_printpnt ("ecgen      pk", &Q, ctx);

  /* Copy the stuff to the key structures. */
  sk->E.model = E->model;
  sk->E.dialect = E->dialect;
  sk->E.p = mpi_copy (E->p);
  sk->E.a = mpi_copy (E->a);
  sk->E.b = mpi_copy (E->b);
  point_init (&sk->E.G);
  point_set (&sk->E.G, &E->G);
  sk->E.n = mpi_copy (E->n);
  point_init (&sk->Q);
  point_set (&sk->Q, &Q);

 leave:
  gcry_mpi_release (a);
  gcry_mpi_release (x);
  gcry_mpi_release (y);
  gcry_free (hash_d);
  return rc;
}


/* Compute an EdDSA signature. See:
 *   [ed25519] 23pp. (PDF) Daniel J. Bernstein, Niels Duif, Tanja
 *   Lange, Peter Schwabe, Bo-Yin Yang. High-speed high-security
 *   signatures.  Journal of Cryptographic Engineering 2 (2012), 77-89.
 *   Document ID: a1a62a2f76d23f65d622484ddd09caf8.
 *   URL: http://cr.yp.to/papers.html#ed25519. Date: 2011.09.26.
 *
 * Despite that this function requires the specification of a hash
 * algorithm, we only support what has been specified by the paper.
 * This may change in the future.  Note that we don't check the used
 * curve; the user is responsible to use Ed25519.
 *
 * Return the signature struct (r,s) from the message hash.  The caller
 * must have allocated R_R and S.
 */
static gpg_err_code_t
sign_eddsa (gcry_mpi_t input, ECC_secret_key *skey,
            gcry_mpi_t r_r, gcry_mpi_t s, int hashalgo, gcry_mpi_t pk)
{
  int rc;
  mpi_ec_t ctx = NULL;
  int b = 256/8;             /* The only size we currently support.  */
  unsigned int tmp;
  unsigned char hash_d[64];  /* Fixme: malloc in secure memory */
  unsigned char digest[64];
  gcry_buffer_t hvec[3];
  const void *mbuf;
  size_t mlen;
  unsigned char *rawmpi = NULL;
  unsigned int rawmpilen;
  unsigned char *encpk = NULL; /* Encoded public key.  */
  unsigned int encpklen;
  mpi_point_struct I;          /* Intermediate value.  */
  mpi_point_struct Q;          /* Public key.  */
  gcry_mpi_t a, x, y, r;

  memset (hvec, 0, sizeof hvec);

  if (!mpi_is_opaque (input))
    return GPG_ERR_INV_DATA;
  if (hashalgo != GCRY_MD_SHA512)
    return GPG_ERR_DIGEST_ALGO;

  /* Initialize some helpers.  */
  point_init (&I);
  point_init (&Q);
  a = mpi_snew (0);
  x = mpi_new (0);
  y = mpi_new (0);
  r = mpi_new (0);
  ctx = _gcry_mpi_ec_p_internal_new (skey->E.model, skey->E.dialect,
                                     skey->E.p, skey->E.a, skey->E.b);

  /* Hash the secret key.  We clear DIGEST so we can use it to left
     pad the key with zeroes for hashing.  */
  rawmpi = _gcry_mpi_get_buffer (skey->d, 0, &rawmpilen, NULL);
  if (!rawmpi)
    {
      rc = gpg_err_code_from_syserror ();
      goto leave;
    }
  memset (digest, 0, b);
  hvec[0].data = digest;
  hvec[0].off = 0;
  hvec[0].len = b > rawmpilen? b - rawmpilen : 0;
  hvec[1].data = rawmpi;
  hvec[1].off = 0;
  hvec[1].len = rawmpilen;
  rc = _gcry_md_hash_buffers (hashalgo, 0, hash_d, hvec, 2);
  gcry_free (rawmpi); rawmpi = NULL;
  if (rc)
    goto leave;

  /* Compute the A value (this modifies hash_d).  */
  reverse_buffer (hash_d, 32);  /* Only the first half of the hash.  */
  hash_d[0] = (hash_d[0] & 0x7f) | 0x40;
  hash_d[31] &= 0xf8;
  _gcry_mpi_set_buffer (a, hash_d, 32, 0);
  /* log_printmpi ("     a", a); */

  /* Compute the public key if it has not been supplied as optional
     parameter.  */
  if (pk)
    {
      rc = eddsa_decodepoint (pk, b, ctx, &Q,  &encpk, &encpklen);
      if (rc)
        goto leave;
      if (DBG_CIPHER)
        log_printhex ("* e_pk", encpk, encpklen);
      if (!_gcry_mpi_ec_curve_point (&Q, ctx))
        {
          rc = GPG_ERR_BROKEN_PUBKEY;
          goto leave;
        }
    }
  else
    {
      _gcry_mpi_ec_mul_point (&Q, a, &skey->E.G, ctx);
      rc = eddsa_encodepoint (&Q, b, ctx, x, y, &encpk, &encpklen);
      if (rc)
        goto leave;
      if (DBG_CIPHER)
        log_printhex ("  e_pk", encpk, encpklen);
    }

  /* Compute R.  */
  mbuf = gcry_mpi_get_opaque (input, &tmp);
  mlen = (tmp +7)/8;
  if (DBG_CIPHER)
    log_printhex ("     m", mbuf, mlen);

  hvec[0].data = hash_d;
  hvec[0].off  = 32;
  hvec[0].len  = 32;
  hvec[1].data = (char*)mbuf;
  hvec[1].len  = mlen;
  rc = _gcry_md_hash_buffers (hashalgo, 0, digest, hvec, 2);
  if (rc)
    goto leave;
  reverse_buffer (digest, 64);
  if (DBG_CIPHER)
    log_printhex ("     r", digest, 64);
  _gcry_mpi_set_buffer (r, digest, 64, 0);
  _gcry_mpi_ec_mul_point (&I, r, &skey->E.G, ctx);
  if (DBG_CIPHER)
    log_printpnt ("   r", &I, ctx);

  /* Convert R into affine coordinates and apply encoding.  */
  rc = eddsa_encodepoint (&I, b, ctx, x, y, &rawmpi, &rawmpilen);
  if (rc)
    goto leave;
  if (DBG_CIPHER)
    log_printhex ("   e_r", rawmpi, rawmpilen);

  /* S = r + a * H(encodepoint(R) + encodepoint(pk) + m) mod n  */
  hvec[0].data = rawmpi;  /* (this is R) */
  hvec[0].off  = 0;
  hvec[0].len  = rawmpilen;
  hvec[1].data = encpk;
  hvec[1].off  = 0;
  hvec[1].len  = encpklen;
  hvec[2].data = (char*)mbuf;
  hvec[2].off  = 0;
  hvec[2].len  = mlen;
  rc = _gcry_md_hash_buffers (hashalgo, 0, digest, hvec, 3);
  if (rc)
    goto leave;

  /* No more need for RAWMPI thus we now transfer it to R_R.  */
  gcry_mpi_set_opaque (r_r, rawmpi, rawmpilen*8);
  rawmpi = NULL;

  reverse_buffer (digest, 64);
  if (DBG_CIPHER)
    log_printhex (" H(R+)", digest, 64);
  _gcry_mpi_set_buffer (s, digest, 64, 0);
  mpi_mulm (s, s, a, skey->E.n);
  mpi_addm (s, s, r, skey->E.n);
  rc = eddsa_encodempi (s, b, &rawmpi, &rawmpilen);
  if (rc)
    goto leave;
  if (DBG_CIPHER)
    log_printhex ("   e_s", rawmpi, rawmpilen);
  gcry_mpi_set_opaque (s, rawmpi, rawmpilen*8);
  rawmpi = NULL;

  rc = 0;

 leave:
  gcry_mpi_release (a);
  gcry_mpi_release (x);
  gcry_mpi_release (y);
  gcry_mpi_release (r);
  _gcry_mpi_ec_free (ctx);
  point_free (&I);
  point_free (&Q);
  gcry_free (encpk);
  gcry_free (rawmpi);
  return rc;
}


/* Verify an EdDSA signature.  See sign_eddsa for the reference.
 * Check if R_IN and S_IN verifies INPUT.  PKEY has the curve
 * parameters and PK is the EdDSA style encoded public key.
 */
static gpg_err_code_t
verify_eddsa (gcry_mpi_t input, ECC_public_key *pkey,
              gcry_mpi_t r_in, gcry_mpi_t s_in, int hashalgo, gcry_mpi_t pk)
{
  int rc;
  mpi_ec_t ctx = NULL;
  int b = 256/8;               /* The only size we currently support.  */
  unsigned int tmp;
  mpi_point_struct Q;          /* Public key.  */
  unsigned char *encpk = NULL; /* Encoded public key.  */
  unsigned int encpklen;
  const void *mbuf, *rbuf;
  unsigned char *tbuf = NULL;
  size_t mlen, rlen;
  unsigned int tlen;
  unsigned char digest[64];
  gcry_buffer_t hvec[3];
  gcry_mpi_t h, s;
  mpi_point_struct Ia, Ib;

  if (!mpi_is_opaque (input) || !mpi_is_opaque (r_in) || !mpi_is_opaque (s_in))
    return GPG_ERR_INV_DATA;
  if (hashalgo != GCRY_MD_SHA512)
    return GPG_ERR_DIGEST_ALGO;

  point_init (&Q);
  point_init (&Ia);
  point_init (&Ib);
  h = mpi_new (0);
  s = mpi_new (0);

  ctx = _gcry_mpi_ec_p_internal_new (pkey->E.model, pkey->E.dialect,
                                     pkey->E.p, pkey->E.a, pkey->E.b);

  /* Decode and check the public key.  */
  rc = eddsa_decodepoint (pk, b, ctx, &Q, &encpk, &encpklen);
  if (rc)
    goto leave;
  if (!_gcry_mpi_ec_curve_point (&Q, ctx))
    {
      rc = GPG_ERR_BROKEN_PUBKEY;
      goto leave;
    }
  if (DBG_CIPHER)
    log_printhex ("  e_pk", encpk, encpklen);
  if (encpklen != b)
    {
      rc = GPG_ERR_INV_LENGTH;
      goto leave;
    }

  /* Convert the other input parameters.  */
  mbuf = gcry_mpi_get_opaque (input, &tmp);
  mlen = (tmp +7)/8;
  if (DBG_CIPHER)
    log_printhex ("     m", mbuf, mlen);
  rbuf = gcry_mpi_get_opaque (r_in, &tmp);
  rlen = (tmp +7)/8;
  if (DBG_CIPHER)
    log_printhex ("     r", rbuf, rlen);
  if (rlen != b)
    {
      rc = GPG_ERR_INV_LENGTH;
      goto leave;
    }

  /* h = H(encodepoint(R) + encodepoint(pk) + m)  */
  hvec[0].data = (char*)rbuf;
  hvec[0].off  = 0;
  hvec[0].len  = rlen;
  hvec[1].data = encpk;
  hvec[1].off  = 0;
  hvec[1].len  = encpklen;
  hvec[2].data = (char*)mbuf;
  hvec[2].off  = 0;
  hvec[2].len  = mlen;
  rc = _gcry_md_hash_buffers (hashalgo, 0, digest, hvec, 3);
  if (rc)
    goto leave;
  reverse_buffer (digest, 64);
  if (DBG_CIPHER)
    log_printhex (" H(R+)", digest, 64);
  _gcry_mpi_set_buffer (h, digest, 64, 0);

  /* According to the paper the best way for verification is:
         encodepoint(sG - h·Q) = encodepoint(r)
     because we don't need to decode R. */
  {
    void *sbuf;
    unsigned int slen;

    sbuf = _gcry_mpi_get_opaque_copy (s_in, &tmp);
    slen = (tmp +7)/8;
    reverse_buffer (sbuf, slen);
    if (DBG_CIPHER)
      log_printhex ("     s", sbuf, slen);
    _gcry_mpi_set_buffer (s, sbuf, slen, 0);
    gcry_free (sbuf);
    if (slen != b)
      {
        rc = GPG_ERR_INV_LENGTH;
        goto leave;
      }
  }

  _gcry_mpi_ec_mul_point (&Ia, s, &pkey->E.G, ctx);
  _gcry_mpi_ec_mul_point (&Ib, h, &Q, ctx);
  _gcry_mpi_neg (Ib.x, Ib.x);
  _gcry_mpi_ec_add_points (&Ia, &Ia, &Ib, ctx);
  rc = eddsa_encodepoint (&Ia, b, ctx, s, h, &tbuf, &tlen);
  if (rc)
    goto leave;
  if (tlen != rlen || memcmp (tbuf, rbuf, tlen))
    {
      if (DBG_CIPHER)
        log_debug ("eddsa verify: Not verified\n");
      rc = GPG_ERR_BAD_SIGNATURE;
      goto leave;
    }

  if (DBG_CIPHER)
    log_debug ("eddsa verify: Accepted\n");
  rc = 0;

 leave:
  gcry_free (encpk);
  gcry_free (tbuf);
  _gcry_mpi_ec_free (ctx);
  gcry_mpi_release (s);
  gcry_mpi_release (h);
  point_free (&Ia);
  point_free (&Ib);
  point_free (&Q);
  return rc;
}



/*********************************************
 **************  interface  ******************
 *********************************************/

static gcry_err_code_t
ecc_generate (int algo, unsigned int nbits, unsigned long evalue,
              const gcry_sexp_t genparms, gcry_sexp_t *r_skey)
{
  gpg_err_code_t rc;
  elliptic_curve_t E;
  ECC_secret_key sk;
  gcry_mpi_t x = NULL;
  gcry_mpi_t y = NULL;
  char *curve_name = NULL;
  gcry_sexp_t l1;
  int transient_key = 0;
  gcry_random_level_t random_level;
  mpi_ec_t ctx = NULL;
  gcry_sexp_t curve_info = NULL;
  gcry_mpi_t base = NULL;
  gcry_mpi_t public = NULL;
  gcry_mpi_t secret = NULL;

  (void)algo;
  (void)evalue;

  memset (&E, 0, sizeof E);
  memset (&sk, 0, sizeof sk);

  if (genparms)
    {
      /* Parse the optional "curve" parameter. */
      l1 = gcry_sexp_find_token (genparms, "curve", 0);
      if (l1)
        {
          curve_name = _gcry_sexp_nth_string (l1, 1);
          gcry_sexp_release (l1);
          if (!curve_name)
            return GPG_ERR_INV_OBJ; /* No curve name or value too large. */
        }

      /* Parse the optional transient-key flag.  */
      l1 = gcry_sexp_find_token (genparms, "transient-key", 0);
      if (l1)
        {
          transient_key = 1;
          gcry_sexp_release (l1);
        }
    }

  /* NBITS is required if no curve name has been given.  */
  if (!nbits && !curve_name)
    return GPG_ERR_NO_OBJ; /* No NBITS parameter. */

  rc = _gcry_ecc_fill_in_curve (nbits, curve_name, &E, &nbits);
  gcry_free (curve_name); curve_name = NULL;
  if (rc)
    goto leave;

  if (DBG_CIPHER)
    {
      log_debug ("ecgen curve info: %s/%s\n",
                 _gcry_ecc_model2str (E.model),
                 _gcry_ecc_dialect2str (E.dialect));
      if (E.name)
        log_debug ("ecgen curve used: %s\n", E.name);
      log_printmpi ("ecgen curve   p", E.p);
      log_printmpi ("ecgen curve   a", E.a);
      log_printmpi ("ecgen curve   b", E.b);
      log_printmpi ("ecgen curve   n", E.n);
      log_printpnt ("ecgen curve G", &E.G, NULL);
    }

  random_level = transient_key ? GCRY_STRONG_RANDOM : GCRY_VERY_STRONG_RANDOM;
  ctx = _gcry_mpi_ec_p_internal_new (E.model, E.dialect, E.p, E.a, E.b);
  x = mpi_new (0);
  y = mpi_new (0);

  switch (E.dialect)
    {
    case ECC_DIALECT_STANDARD:
      rc = nist_generate_key (&sk, &E, ctx, random_level, nbits);
      break;
    case ECC_DIALECT_ED25519:
      rc = eddsa_generate_key (&sk, &E, ctx, random_level);
      break;
    default:
      rc = GPG_ERR_INTERNAL;
      break;
    }
  if (rc)
    goto leave;

  /* Copy data to the result.  */
  if (_gcry_mpi_ec_get_affine (x, y, &sk.E.G, ctx))
    log_fatal ("ecgen: Failed to get affine coordinates for %s\n", "G");
  base = _gcry_ecc_ec2os (x, y, sk.E.p);
  if (sk.E.dialect == ECC_DIALECT_ED25519)
    {
      unsigned char *encpk;
      unsigned int encpklen;

      rc = eddsa_encodepoint (&sk.Q, 256/8, ctx, x, y, &encpk, &encpklen);
      if (rc)
        return rc;
      public = mpi_new (0);
      gcry_mpi_set_opaque (public, encpk, encpklen*8);
      encpk = NULL;
    }
  else
    {
      if (_gcry_mpi_ec_get_affine (x, y, &sk.Q, ctx))
        log_fatal ("ecgen: Failed to get affine coordinates for %s\n", "Q");
      public = _gcry_ecc_ec2os (x, y, sk.E.p);
    }
  secret = sk.d; sk.d = NULL;
  if (E.name)
    {
      rc = gcry_sexp_build (&curve_info, NULL, "(curve %s)", E.name);
      if (rc)
        goto leave;
    }

  rc = gcry_sexp_build (r_skey, NULL,
                        "(key-data"
                        " (public-key"
                        "  (ecc%S(p%m)(a%m)(b%m)(g%m)(n%m)(q%m)))"
                        " (private-key"
                        "  (ecc%S(p%m)(a%m)(b%m)(g%m)(n%m)(q%m)(d%m)))"
                        " )",
                        curve_info,
                        sk.E.p, sk.E.a, sk.E.b, base, sk.E.n, public,
                        curve_info,
                        sk.E.p, sk.E.a, sk.E.b, base, sk.E.n, public, secret);
  if (rc)
    goto leave;

  if (DBG_CIPHER)
    {
      log_printmpi ("ecgen result  p", sk.E.p);
      log_printmpi ("ecgen result  a", sk.E.a);
      log_printmpi ("ecgen result  b", sk.E.b);
      log_printmpi ("ecgen result  G", base);
      log_printmpi ("ecgen result  n", sk.E.n);
      log_printmpi ("ecgen result  Q", public);
      log_printmpi ("ecgen result  d", secret);
    }

 leave:
  mpi_free (secret);
  mpi_free (public);
  mpi_free (base);
  {
    _gcry_ecc_curve_free (&sk.E);
    point_free (&sk.Q);
    mpi_free (sk.d);
  }
  _gcry_ecc_curve_free (&E);
  mpi_free (x);
  mpi_free (y);
  _gcry_mpi_ec_free (ctx);
  gcry_sexp_release (curve_info);
  return rc;
}


static gcry_err_code_t
ecc_check_secret_key (int algo, gcry_mpi_t *skey)
{
  gpg_err_code_t err;
  ECC_secret_key sk;

  (void)algo;

  /* FIXME:  This check looks a bit fishy:  Now long is the array?  */
  if (!skey[0] || !skey[1] || !skey[2] || !skey[3] || !skey[4] || !skey[5]
      || !skey[6])
    return GPG_ERR_BAD_MPI;

  sk.E.model = MPI_EC_WEIERSTRASS;
  sk.E.p = skey[0];
  sk.E.a = skey[1];
  sk.E.b = skey[2];
  point_init (&sk.E.G);
  err = _gcry_ecc_os2ec (&sk.E.G, skey[3]);
  if (err)
    {
      point_free (&sk.E.G);
      return err;
    }
  sk.E.n = skey[4];
  point_init (&sk.Q);
  err = _gcry_ecc_os2ec (&sk.Q, skey[5]);
  if (err)
    {
      point_free (&sk.E.G);
      point_free (&sk.Q);
      return err;
    }

  {
    const unsigned char *buf;
    unsigned int n;

    gcry_assert (mpi_is_opaque (skey[6]));

    buf = gcry_mpi_get_opaque (skey[6], &n);
    if (!buf)
      err = GPG_ERR_INV_OBJ;
    else
      {
        n = (n + 7)/8;
        sk.d = NULL;
        err = gcry_mpi_scan (&sk.d, GCRYMPI_FMT_USG, buf, n, NULL);
        if (!err)
          {
            if (check_secret_key (&sk))
              err = GPG_ERR_BAD_SECKEY;
            gcry_mpi_release (sk.d);
            sk.d = NULL;
          }
      }
  }

  point_free (&sk.E.G);
  point_free (&sk.Q);
  return err;
}


static gcry_err_code_t
ecc_sign (int algo, gcry_sexp_t *r_result, gcry_mpi_t data, gcry_mpi_t *skey,
          int flags, int hashalgo)
{
  gpg_err_code_t rc;
  ECC_secret_key sk;
  gcry_mpi_t r, s;

  (void)algo;

  if (!data || !skey[0] || !skey[1] || !skey[2] || !skey[3] || !skey[4]
      || !skey[6] )
    return GPG_ERR_BAD_MPI;

  sk.E.model = ((flags & PUBKEY_FLAG_EDDSA)
                ? MPI_EC_TWISTEDEDWARDS
                : MPI_EC_WEIERSTRASS);
  sk.E.p = skey[0];
  sk.E.a = skey[1];
  sk.E.b = skey[2];
  point_init (&sk.E.G);
  sk.Q.x = NULL;
  sk.Q.y = NULL;
  sk.Q.z = NULL;
  rc = _gcry_ecc_os2ec (&sk.E.G, skey[3]);
  if (rc)
    {
      point_free (&sk.E.G);
      return rc;
    }
  sk.E.n = skey[4];

  r = mpi_alloc (mpi_get_nlimbs (sk.E.p));
  s = mpi_alloc (mpi_get_nlimbs (sk.E.p));

  {
    const unsigned char *buf;
    unsigned int n;

    gcry_assert (mpi_is_opaque (skey[6]));

    buf = gcry_mpi_get_opaque (skey[6], &n);
    if (!buf)
      rc = GPG_ERR_INV_OBJ;
    else
      {
        n = (n + 7)/8;
        sk.d = NULL;
        rc = gcry_mpi_scan (&sk.d, GCRYMPI_FMT_USG, buf, n, NULL);
        if (!rc)
          {
            if ((flags & PUBKEY_FLAG_EDDSA))
              {
                rc = sign_eddsa (data, &sk, r, s, hashalgo, skey[5]);
                if (!rc)
                  rc = gcry_sexp_build (r_result, NULL,
                                        "(sig-val(eddsa(r%M)(s%M)))", r, s);
              }
            else
              {
                rc = sign_ecdsa (data, &sk, r, s, flags, hashalgo);
                if (!rc)
                  rc = gcry_sexp_build (r_result, NULL,
                                        "(sig-val(ecdsa(r%M)(s%M)))", r, s);
              }
            gcry_mpi_release (sk.d);
            sk.d = NULL;
          }
      }
  }

  mpi_free (r);
  mpi_free (s);
  point_free (&sk.E.G);
  if (sk.Q.x)
    point_free (&sk.Q);
  return rc;
}


static gcry_err_code_t
ecc_verify (int algo, gcry_mpi_t hash, gcry_mpi_t *data, gcry_mpi_t *pkey,
            int (*cmp)(void *, gcry_mpi_t), void *opaquev,
            int flags, int hashalgo)
{
  gpg_err_code_t err;
  ECC_public_key pk;

  (void)algo;
  (void)cmp;
  (void)opaquev;

  if (!data[0] || !data[1] || !hash || !pkey[0] || !pkey[1] || !pkey[2]
      || !pkey[3] || !pkey[4] || !pkey[5] )
    return GPG_ERR_BAD_MPI;

  pk.E.model = ((flags & PUBKEY_FLAG_EDDSA)
                ? MPI_EC_TWISTEDEDWARDS
                : MPI_EC_WEIERSTRASS);
  pk.E.p = pkey[0];
  pk.E.a = pkey[1];
  pk.E.b = pkey[2];
  point_init (&pk.E.G);
  err = _gcry_ecc_os2ec (&pk.E.G, pkey[3]);
  if (err)
    {
      point_free (&pk.E.G);
      return err;
    }
  pk.E.n = pkey[4];

  if ((flags & PUBKEY_FLAG_EDDSA))
    {
      pk.Q.x = NULL;
      pk.Q.y = NULL;
      pk.Q.z = NULL;

      err = verify_eddsa (hash, &pk, data[0], data[1], hashalgo, pkey[5]);
    }
  else
    {
      point_init (&pk.Q);
      err = _gcry_ecc_os2ec (&pk.Q, pkey[5]);
      if (err)
        {
          point_free (&pk.E.G);
          point_free (&pk.Q);
          return err;
        }

      if (mpi_is_opaque (hash))
        {
          const void *abuf;
          unsigned int abits, qbits;
          gcry_mpi_t a;

          qbits = mpi_get_nbits (pk.E.n);

          abuf = gcry_mpi_get_opaque (hash, &abits);
          err = gcry_mpi_scan (&a, GCRYMPI_FMT_USG, abuf, (abits+7)/8, NULL);
          if (!err)
            {
              if (abits > qbits)
                gcry_mpi_rshift (a, a, abits - qbits);

              err = verify_ecdsa (a, &pk, data[0], data[1]);
              gcry_mpi_release (a);
            }
        }
      else
        err = verify_ecdsa (hash, &pk, data[0], data[1]);
    }

  point_free (&pk.E.G);
  point_free (&pk.Q);
  return err;
}


/* ecdh raw is classic 2-round DH protocol published in 1976.
 *
 * Overview of ecc_encrypt_raw and ecc_decrypt_raw.
 *
 * As with any PK operation, encrypt version uses a public key and
 * decrypt -- private.
 *
 * Symbols used below:
 *     G - field generator point
 *     d - private long-term scalar
 *    dG - public long-term key
 *     k - ephemeral scalar
 *    kG - ephemeral public key
 *   dkG - shared secret
 *
 * ecc_encrypt_raw description:
 *   input:
 *     data[0] : private scalar (k)
 *   output: A new S-expression with the parameters:
 *     s : shared point (kdG)
 *     e : generated ephemeral public key (kG)
 *
 * ecc_decrypt_raw description:
 *   input:
 *     data[0] : a point kG (ephemeral public key)
 *   output:
 *     result[0] : shared point (kdG)
 */
static gcry_err_code_t
ecc_encrypt_raw (int algo, gcry_sexp_t *r_result, gcry_mpi_t k,
                 gcry_mpi_t *pkey, int flags)
{
  gpg_err_code_t rc;
  ECC_public_key pk;
  mpi_ec_t ctx;
  gcry_mpi_t s, e;

  (void)algo;
  (void)flags;

  if (!k
      || !pkey[0] || !pkey[1] || !pkey[2] || !pkey[3] || !pkey[4] || !pkey[5])
    return GPG_ERR_BAD_MPI;

  pk.E.model = MPI_EC_WEIERSTRASS;
  pk.E.p = pkey[0];
  pk.E.a = pkey[1];
  pk.E.b = pkey[2];
  point_init (&pk.E.G);
  rc = _gcry_ecc_os2ec (&pk.E.G, pkey[3]);
  if (rc)
    {
      point_free (&pk.E.G);
      return rc;
    }
  pk.E.n = pkey[4];
  point_init (&pk.Q);
  rc = _gcry_ecc_os2ec (&pk.Q, pkey[5]);
  if (rc)
    {
      point_free (&pk.E.G);
      point_free (&pk.Q);
      return rc;
    }

  ctx = _gcry_mpi_ec_p_internal_new (pk.E.model, pk.E.dialect,
                                     pk.E.p, pk.E.a, pk.E.b);
  s = mpi_alloc (mpi_get_nlimbs (pk.E.p));
  e = mpi_alloc (mpi_get_nlimbs (pk.E.p));

  /* The following is false: assert( mpi_cmp_ui( R.x, 1 )==0 );, so */
  {
    mpi_point_struct R;  /* Result that we return.  */
    gcry_mpi_t x, y;

    x = mpi_new (0);
    y = mpi_new (0);

    point_init (&R);

    /* R = kQ  <=>  R = kdG  */
    _gcry_mpi_ec_mul_point (&R, k, &pk.Q, ctx);

    if (_gcry_mpi_ec_get_affine (x, y, &R, ctx))
      log_fatal ("ecdh: Failed to get affine coordinates for kdG\n");
    s = _gcry_ecc_ec2os (x, y, pk.E.p);

    /* R = kG */
    _gcry_mpi_ec_mul_point (&R, k, &pk.E.G, ctx);

    if (_gcry_mpi_ec_get_affine (x, y, &R, ctx))
      log_fatal ("ecdh: Failed to get affine coordinates for kG\n");
    e = _gcry_ecc_ec2os (x, y, pk.E.p);

    mpi_free (x);
    mpi_free (y);

    point_free (&R);
  }

  _gcry_mpi_ec_free (ctx);
  point_free (&pk.E.G);
  point_free (&pk.Q);

  rc = gcry_sexp_build (r_result, NULL, "(enc-val(ecdh(s%m)(e%m)))", s, e);
  mpi_free (s);
  mpi_free (e);

  return rc;
}

/*  input:
 *     data[0] : a point kG (ephemeral public key)
 *   output:
 *     resaddr[0] : shared point kdG
 *
 *  see ecc_encrypt_raw for details.
 */
static gcry_err_code_t
ecc_decrypt_raw (int algo, gcry_sexp_t *r_plain, gcry_mpi_t *data,
                 gcry_mpi_t *skey, int flags,
                 enum pk_encoding encoding, int hash_algo,
                 unsigned char *label, size_t labellen)
{
  gpg_err_code_t rc;
  ECC_secret_key sk;
  mpi_point_struct R;	/* Result that we return.  */
  mpi_point_struct kG;
  mpi_ec_t ctx;
  gcry_mpi_t r;

  (void)algo;
  (void)flags;
  (void)encoding;
  (void)hash_algo;
  (void)label;
  (void)labellen;

  if (!data || !data[0]
      || !skey[0] || !skey[1] || !skey[2] || !skey[3] || !skey[4]
      || !skey[5] || !skey[6] )
    return GPG_ERR_BAD_MPI;

  point_init (&kG);
  rc = _gcry_ecc_os2ec (&kG, data[0]);
  if (rc)
    {
      point_free (&kG);
      return rc;
    }

  sk.E.model = MPI_EC_WEIERSTRASS;
  sk.E.p = skey[0];
  sk.E.a = skey[1];
  sk.E.b = skey[2];
  point_init (&sk.E.G);
  rc = _gcry_ecc_os2ec (&sk.E.G, skey[3]);
  if (rc)
    {
      point_free (&kG);
      point_free (&sk.E.G);
      return rc;
    }
  sk.E.n = skey[4];
  point_init (&sk.Q);
  rc = _gcry_ecc_os2ec (&sk.Q, skey[5]);
  if (rc)
    {
      point_free (&kG);
      point_free (&sk.E.G);
      point_free (&sk.Q);
      return rc;
    }
  sk.d = skey[6];

  ctx = _gcry_mpi_ec_p_internal_new (sk.E.model, sk.E.dialect,
                                     sk.E.p, sk.E.a, sk.E.b);

  /* R = dkG */
  point_init (&R);
  _gcry_mpi_ec_mul_point (&R, sk.d, &kG, ctx);

  point_free (&kG);

  /* The following is false: assert( mpi_cmp_ui( R.x, 1 )==0 );, so:  */
  {
    gcry_mpi_t x, y;

    x = mpi_new (0);
    y = mpi_new (0);

    if (_gcry_mpi_ec_get_affine (x, y, &R, ctx))
      log_fatal ("ecdh: Failed to get affine coordinates\n");

    r = _gcry_ecc_ec2os (x, y, sk.E.p);
    if (!r)
      rc = gpg_err_code_from_syserror ();
    else
      rc = 0;
    mpi_free (x);
    mpi_free (y);
  }

  point_free (&R);
  _gcry_mpi_ec_free (ctx);
  point_free (&kG);
  point_free (&sk.E.G);
  point_free (&sk.Q);

  if (!rc)
    rc = gcry_sexp_build (r_plain, NULL, "(value %m)", r);
  mpi_free (r);
  return rc;
}


static unsigned int
ecc_get_nbits (int algo, gcry_mpi_t *pkey)
{
  (void)algo;

  return mpi_get_nbits (pkey[0]);
}


/* See rsa.c for a description of this function.  */
static gpg_err_code_t
compute_keygrip (gcry_md_hd_t md, gcry_sexp_t keyparam)
{
#define N_COMPONENTS 6
  static const char names[N_COMPONENTS+1] = "pabgnq";
  gpg_err_code_t ec = 0;
  gcry_sexp_t l1;
  gcry_mpi_t values[N_COMPONENTS];
  int idx;

  /* Clear the values for easier error cleanup.  */
  for (idx=0; idx < N_COMPONENTS; idx++)
    values[idx] = NULL;

  /* Fill values with all provided parameters.  */
  for (idx=0; idx < N_COMPONENTS; idx++)
    {
      l1 = gcry_sexp_find_token (keyparam, names+idx, 1);
      if (l1)
        {
          values[idx] = gcry_sexp_nth_mpi (l1, 1, GCRYMPI_FMT_USG);
	  gcry_sexp_release (l1);
	  if (!values[idx])
            {
              ec = GPG_ERR_INV_OBJ;
              goto leave;
            }
	}
    }

  /* Check whether a curve parameter is available and use that to fill
     in missing values.  */
  l1 = gcry_sexp_find_token (keyparam, "curve", 5);
  if (l1)
    {
      char *curve;
      gcry_mpi_t tmpvalues[N_COMPONENTS];

      for (idx = 0; idx < N_COMPONENTS; idx++)
        tmpvalues[idx] = NULL;

      curve = _gcry_sexp_nth_string (l1, 1);
      gcry_sexp_release (l1);
      if (!curve)
        {
          ec = GPG_ERR_INV_OBJ; /* Name missing or out of core. */
          goto leave;
        }
      ec = _gcry_ecc_get_param (curve, tmpvalues);
      gcry_free (curve);
      if (ec)
        goto leave;

      for (idx = 0; idx < N_COMPONENTS; idx++)
        {
          if (!values[idx])
            values[idx] = tmpvalues[idx];
          else
            mpi_free (tmpvalues[idx]);
        }
    }

  /* Check that all parameters are known and normalize all MPIs (that
     should not be required but we use an internal function later and
     thus we better make 100% sure that they are normalized). */
  for (idx = 0; idx < N_COMPONENTS; idx++)
    if (!values[idx])
      {
        ec = GPG_ERR_NO_OBJ;
        goto leave;
      }
    else
      _gcry_mpi_normalize (values[idx]);

  /* Hash them all.  */
  for (idx = 0; idx < N_COMPONENTS; idx++)
    {
      char buf[30];
      unsigned char *rawmpi;
      unsigned int rawmpilen;

      rawmpi = _gcry_mpi_get_buffer (values[idx], 0, &rawmpilen, NULL);
      if (!rawmpi)
        {
          ec = gpg_err_code_from_syserror ();
          goto leave;
        }
      snprintf (buf, sizeof buf, "(1:%c%u:", names[idx], rawmpilen);
      gcry_md_write (md, buf, strlen (buf));
      gcry_md_write (md, rawmpi, rawmpilen);
      gcry_md_write (md, ")", 1);
      gcry_free (rawmpi);
    }

 leave:
  for (idx = 0; idx < N_COMPONENTS; idx++)
    _gcry_mpi_release (values[idx]);

  return ec;
#undef N_COMPONENTS
}



/*
   Low-level API helper functions.
 */

/* This is the wroker function for gcry_pubkey_get_sexp for ECC
   algorithms.  Note that the caller has already stored NULL at
   R_SEXP.  */
gpg_err_code_t
_gcry_pk_ecc_get_sexp (gcry_sexp_t *r_sexp, int mode, mpi_ec_t ec)
{
  gpg_err_code_t rc;
  gcry_mpi_t mpi_G = NULL;
  gcry_mpi_t mpi_Q = NULL;

  if (!ec->p || !ec->a || !ec->b || !ec->G || !ec->n)
    return GPG_ERR_BAD_CRYPT_CTX;

  if (mode == GCRY_PK_GET_SECKEY && !ec->d)
    return GPG_ERR_NO_SECKEY;

  /* Compute the public point if it is missing.  */
  if (!ec->Q && ec->d)
    {
      ec->Q = gcry_mpi_point_new (0);
      _gcry_mpi_ec_mul_point (ec->Q, ec->d, ec->G, ec);
    }

  /* Encode G and Q.  */
  mpi_G = _gcry_mpi_ec_ec2os (ec->G, ec);
  if (!mpi_G)
    {
      rc = GPG_ERR_BROKEN_PUBKEY;
      goto leave;
    }
  if (!ec->Q)
    {
      rc = GPG_ERR_BAD_CRYPT_CTX;
      goto leave;
    }
  mpi_Q = _gcry_mpi_ec_ec2os (ec->Q, ec);
  if (!mpi_Q)
    {
      rc = GPG_ERR_BROKEN_PUBKEY;
      goto leave;
    }

  /* Fixme: We should return a curve name instead of the parameters if
     if know that they match a curve.  */

  if (ec->d && (!mode || mode == GCRY_PK_GET_SECKEY))
    {
      /* Let's return a private key. */
      rc = gcry_sexp_build
        (r_sexp, NULL,
         "(private-key(ecc(p%m)(a%m)(b%m)(g%m)(n%m)(q%m)(d%m)))",
         ec->p, ec->a, ec->b, mpi_G, ec->n, mpi_Q, ec->d);
    }
  else if (ec->Q)
    {
      /* Let's return a public key.  */
      rc = gcry_sexp_build
        (r_sexp, NULL,
         "(public-key(ecc(p%m)(a%m)(b%m)(g%m)(n%m)(q%m)))",
         ec->p, ec->a, ec->b, mpi_G, ec->n, mpi_Q);
    }
  else
    rc = GPG_ERR_BAD_CRYPT_CTX;

 leave:
  mpi_free (mpi_Q);
  mpi_free (mpi_G);
  return rc;
}



/*
     Self-test section.
 */


static gpg_err_code_t
selftests_ecdsa (selftest_report_func_t report)
{
  const char *what;
  const char *errtxt;

  what = "low-level";
  errtxt = NULL; /*selftest ();*/
  if (errtxt)
    goto failed;

  /* FIXME:  need more tests.  */

  return 0; /* Succeeded. */

 failed:
  if (report)
    report ("pubkey", GCRY_PK_ECC, what, errtxt);
  return GPG_ERR_SELFTEST_FAILED;
}


/* Run a full self-test for ALGO and return 0 on success.  */
static gpg_err_code_t
run_selftests (int algo, int extended, selftest_report_func_t report)
{
  (void)extended;

  if (algo != GCRY_PK_ECC)
    return GPG_ERR_PUBKEY_ALGO;

  return selftests_ecdsa (report);
}




static const char *ecc_names[] =
  {
    "ecc",
    "ecdsa",
    "ecdh",
    "eddsa",
    NULL,
  };

gcry_pk_spec_t _gcry_pubkey_spec_ecc =
  {
    GCRY_PK_ECC, { 0, 0 },
    (GCRY_PK_USAGE_SIGN | GCRY_PK_USAGE_ENCR),
    "ECC", ecc_names,
    "pabgnq", "pabgnqd", "sw", "rs", "pabgnq",
    ecc_generate,
    ecc_check_secret_key,
    ecc_encrypt_raw,
    ecc_decrypt_raw,
    ecc_sign,
    ecc_verify,
    ecc_get_nbits,
    run_selftests,
    compute_keygrip,
    _gcry_ecc_get_param,
    _gcry_ecc_get_curve,
    _gcry_ecc_get_param_sexp
  };
