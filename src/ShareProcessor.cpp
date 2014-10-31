/**
 * Implementation of GapMiners pow processing functionality
 *
 * Copyright (C)  2014  The Gapcoin developers  <info@gapcoin.org>
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#ifndef __STDC_FORMAT_MACROS 
#define __STDC_FORMAT_MACROS 
#endif
#ifndef __STDC_LIMIT_MACROS  
#define __STDC_LIMIT_MACROS  
#endif
#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>
#include <vector>
#include "PoWCore/src/PoWProcessor.h"
#include "PoWCore/src/PoW.h"
#include "BlockHeader.h"
#include "ShareProcessor.h"
#include "Rpc.h"
#include "verbose.h"
#include "Opts.h"
#include "Stratum.h"

using namespace std;


/* synchronization mutexes */
pthread_mutex_t ShareProcessor::queue_mutex    = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t ShareProcessor::wait_mutex     = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t ShareProcessor::creation_mutex = PTHREAD_MUTEX_INITIALIZER;

/* indicates if this was initialized */
bool ShareProcessor::initialized = false;

/* indicates that the share thread is running */
bool ShareProcessor::running = true;

/* this will be a singleton */
ShareProcessor *ShareProcessor::only_instance = NULL;


/* return the only instance of this */
ShareProcessor *ShareProcessor::get_processor() {

  pthread_mutex_lock(&creation_mutex);
  if (!initialized) {
    only_instance = new ShareProcessor();
    initialized   = true;
  }
  pthread_mutex_unlock(&creation_mutex);

  return only_instance;
}

/* run clean up */
void ShareProcessor::cleanup() {
  pthread_mutex_lock(&creation_mutex);
  if (initialized) {
    initialized = false;
    delete only_instance;
    only_instance = NULL;
  }
  pthread_mutex_unlock(&creation_mutex);
}

/**
 * Processes a given PoW
 */
bool ShareProcessor::process(PoW *pow) {

  /* stop sieve if we mine stales */
  bool stop_sieveing = true;

  if (header != NULL) {
    BlockHeader *cur = get_header_from(pow);
 
    /* is PoW for the current hash */
    if (cur != NULL) {
 
      pthread_mutex_lock(&queue_mutex);
      shares.push(cur);
 
      if (!running) {
        running = true;
 
        /* wake up share thread */
        pthread_mutex_unlock(&wait_mutex);
      }
      pthread_mutex_unlock(&queue_mutex);
      
      stop_sieveing = false;
    }
  }

  return stop_sieveing;
}

/* updates the current block header */
void ShareProcessor::update_header(BlockHeader *header) {
  pthread_mutex_lock(&queue_mutex);
  
  /* discard now obsolete shares */
  while (!shares.empty()) {
    BlockHeader *cur = shares.front();
    shares.pop();
    delete cur;
  }

  if (header != NULL)
    delete this->header;

  this->header = header->clone();

  pthread_mutex_unlock(&queue_mutex);
}

/* private constructor to allow only one instance */
ShareProcessor::ShareProcessor() {
  this->header = NULL;

  /* lock the shares thread */
  pthread_mutex_lock(&wait_mutex);
  
  pthread_create(&thread, NULL, share_processor, (void *) &shares);
}

/* private destructor */
ShareProcessor::~ShareProcessor() {
  pthread_mutex_lock(&queue_mutex);

  /* clear shares */
  while (!shares.empty()) {
    BlockHeader *cur = shares.front();
    shares.pop();
    delete cur;
  }

  pthread_mutex_unlock(&queue_mutex);

  pthread_join(thread, NULL);

  if (header != NULL)
    delete header;
}

/* thread which processes the share queue */
void *ShareProcessor::share_processor(void *args) {

  queue<BlockHeader *> *shares = (queue<BlockHeader *> *) args;
  BlockHeader *cur;
  NetProtocol *net = NULL;
  bool stratum = Opts::get_instance()->has_stratum();

  if (stratum)
    net = (NetProtocol *) Stratum::get_instance();
  else
    net = (NetProtocol *) Rpc::get_instance();

  while (initialized) {
    
    /* get the next share to process */
    pthread_mutex_lock(&queue_mutex);

    /* break if nothing to do */
    if (shares->empty()) {
      running = false;
      pthread_mutex_unlock(&queue_mutex);

      /* wait till we get aroused */
      pthread_mutex_lock(&wait_mutex);
      continue;
    } else {

      cur = shares->front();
      shares->pop();
      pthread_mutex_unlock(&queue_mutex);
    }

    /* submit share */
    bool accepted = net->sendwork(cur);

    /* send the share to gapcoind */
    if (!stratum) {
      pthread_mutex_lock(&io_mutex);
      cout.precision(7);
      cout << get_time();
      cout << "Found Share: ";
      cout << fixed << (((double) cur->get_pow().difficulty()) 
                                       / TWO_POW48);
      cout << "  =>  " <<  (accepted ? "accepted" : "stale!");
      cout << endl;
      pthread_mutex_unlock(&io_mutex);
    }

    delete cur;
  }

  return NULL;
}

/**
 * test if share is for the current block
 * if true returns the adjusted Block,
 * else NULL
 */
BlockHeader *ShareProcessor::get_header_from(PoW *share) {

  pthread_mutex_lock(&queue_mutex);
  BlockHeader *cur = header->clone();
  pthread_mutex_unlock(&queue_mutex);

  cur->nonce = share->get_nonce(); 
  cur->shift = share->get_shift(); 
  share->get_adder(&cur->adder);

  mpz_t mpz_hhash, mpz_shash;
  mpz_init(mpz_hhash);
  mpz_init(mpz_shash);

  share->get_hash(mpz_shash);
  cur->get_hash(mpz_hhash);

  BlockHeader *ret = NULL;

  /* is share for the current hash */
  if (!mpz_cmp(mpz_shash, mpz_hhash))
    ret = cur;
  else
    delete cur;

  mpz_clear(mpz_shash);
  mpz_clear(mpz_hhash);

  return ret;
}
