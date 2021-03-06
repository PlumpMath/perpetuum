/* runtime.c -- Running around in circles, forever and ever (or until done).
 *
 * These routines operate on the structures of <perpetuum/model.h> to make
 * them process events.  To that end, you may inject_tokens() and have the
 * new additions passed around until the Petri net returns to rest.
 *
 * It is up to an event loop to inject new tokens, and it is up to a library
 * of actions to cause all sorts of activity to be initiated.  Note the
 * distinction between an action (a snappy function call that quickly
 * returns whether it succeeded in sparking off whatever was desired) and
 * activities (which, conformant to UML lingo, takes time to come from
 * start to end).  Perpatuum facilitates the activity concept through a
 * couple of separate actions with intermediate state being stored.  This
 * state could be something like an LDAP request identifier, used to
 * recognised a response as paired with a request.
 *
 * The model processing logic in this file only concerns itself with the
 * quick actions, and leaves the more advanced handling of activities to
 * surrounding libraries.  Note that Perpetuum logic follows Petri nets
 * in having no notion of atomic, long-lived actions.  They would cause
 * locking behaviour and all the complexity that comes with it.
 *
 * The code below is designed to be part of the event loop.  Specifically,
 * it is not re-entrant; the Perpetuum model intends to distribute work
 * as threads or processes, but thanks to quick actions it does not need
 * to be concurrent internally; this is a strength or weakness, dependent
 * on your view on life... perhaps atomic operations can be used to ease
 * this choice in later revisions.
 *
 * From: Rick van Rein <rick@openfortress.nl>
 */


#include <stdint.h>

#include <assert.h>

#include <time.h>

#include <perpetuum/model.h>


#ifndef BUILD_SINGLE_THREADED
#  error "We only guarantee proper functioning for single-threaded builds!"
#  include "opa_primitives.h"
#endif


/* Following are a number of atomic operations when we requie concurrency;
 * the operations become plain operations in other circumstances.  Using
 * the macros in the code below makes them lock-free concurrent.
 *
 * We never block-lock, but always try-lock.  This may fail when multiple
 * transitions are trying to grab tokens from a place at the same time.
 * The response to a failed try-lock is to withdraw from the attemp to
 * fire a transition.
 *
 * In general, this need to withdraw only arises when threads are racing
 * against each other to get to tokens.  Under normal circumstances,
 * trying to lock a place will lead to withdrawal of a token offer from
 * transitions, causing their countdown to increment again.
 *
 * Places will not communicate every change to the transitions following
 * them; they only share the important changes, notable when the number
 * of "available" tokens (including locked ones) changes from 0 to 1 or
 * opposite on inhibitor arcs; and when the number of "unlocked" tokens
 * (which excludes locked ones) increments form 1 to 0 or opposite on
 * standard/positive arcs.
 *
 * We will use atomic operations on all the counters that may be "grabbed"
 * from multiple threads at the same time (and will have them trivialise
 * to plain operations when CONFIG_SINGLE_THREADED is configured, as is
 * strongly advised).  Atomic operations are likely to place demands on
 * the sizes of integers, but then again on platforms where it is used
 * that should not be a matter of concern.
 *
 * All this guarantees absense of deadlock.  We have no concerns in that
 * respect.  What we are more worried about, is whether we might end up in
 * livelock sitations, starvation or who knows what else.  This is a
 * topic of further research, hence the #warning above.
 *
 * Note: These macros are only use d below, they are not a generic API.
 *       Because of this, we have not had a lot of zeal in placing bracing.
 */


#ifdef BUILD_SINGLE_THREADED

/* The atomic operations from OpenPA make us lock-free yet stable */
# define cas_int(intptr,old,new) OPA_cas_int  ((OPA_int_t *) intptr, old, new)
# define xcg_int(intptr,new)     OPA_swap_int ((OPA_int_t *) intptr, new)
# define set_int(intptr,new)     OPA_store_ptr ((OPA_int_t *) intptr, new)
# define get_int(intptr)         OPA_load_int ((OPA_int_t *) intptr)
# define is_zero(intptr)         (0 == get_int (intptr))

#else /* CONFIG_SINGLE_THREADED */

/* No need for atomic operations when we are sure to have only one thread */

static int _tmp;
#define cas_int(intptr,old,new) ((*intptr == old) \
				? (*intptr=new, old) \
				: (*intptr))
#define xcg_int(intptr,new)	(_tmp = (*intptr), (*intptr)=new, _tmp)
#define set_int(intptr,new)	(*intptr = new)
#define get_int(intptr)		(*intptr)
#define is_zero(intptr)		(0 == get_int (intptr))

#endif /* CONFIG_SINGLE_THREADED */


//TODO// Will at some point move into a header file

//TODO// Maybe worthwhile to have outer API calls with pnc_t and inner with stat/dyn/tables "unpacked" -- or can we arrange things with more clever data structures?  There is a *lot* of indirection going on now... having SINGLETON_PETRI_NETS support will definitaly help in that case
//TODO// It seems useful to have a few macros for lookup, and very those depending on the variant being compiled


/* Mark a place with the given number of additional tokens.  This may be
 * used to place an initial marking [which has no use being done atomically,
 * since Petri nets are non-deterministic semantics anyway] as well as to
 * add tokens that result from a transition firing.
 */
void mark_place (petrinet_colour_t *pnc, placeref_t plc, tokenctr_t incr) {
	bool try_trans (petrinet_colour_t *pnc, transref_t trit, placeref_t exempt); //TODO// Move to header file?  Internal/external versions?
	tokenctr_t exempted = 0;
	//
	// Increment our token counter; if it goes from 0 to higher, we
	// have changed fundamnetally, and will do two things:
	//  1. We remove a token from each inhibited transition following us
	//  2. We offer a token to each following transition that only needs 1
	//  3. We increment our offering to each following transition
	//
	// When we find a following place that only needed one token, we try
	// to fire that transition immediately.  For reasons of fairness, we
	// start this 2nd phase from a random spot.  When firing succeeds, we
	// stop offering the token; if it fails, we continue as planned.
	//
	// We should care about some non-determinism.  A deterministic order
	// of handling would still be a valid implementation, but unnatural.
	// So we tap into a global resource that scatters our starting point.
	// We do however make just a single round of failed attempts during 2.
	place_colour_t *dyn = &pnc->place_ary [plc];
	place_t *stat = &pnc->topology->place_ary [plc];
	//
	// Grab the old value -- only if it was 0 should we get really active
	assert (incr > 0);
	int old;
	do {
		old = get_int (&dyn->available);
	} while (cas_int (&dyn->available, old, old+incr) != old);
	if (old == 0) {
		//
		// Block all followers over an inhibiting arc because it has
		// just become a hindrance to these followers
		transref_t here;
		for (here=stat->trans_out_inh [0]; here>0; here--) {
			trans_t inh = stat->trans_out_inh [here];
			inc_int (&pnc->trans_ary [inh].countdown);
		}
	}
#ifndef BUILD_SINGLE_THREADED
	//
	// When multi-threading, we also have an unlocked counter, and it
	// must be lowered.  In deciding whether to try firing transitions
	// we need to look at this value.
	do {
		old = get_int (&dyn->unlocked);
	} while (cas_int (&dyn->unlocked, old, old+incr) != old);
#else
	//
	// If single threading: Fallthrough with old set from dyn->available
	// because we don't have a separate unlocked counter, it is the same
#endif
	//
	// When our old token count was already > 0 then we have nothing to do;
	// inhibitors will have been applied, countdowns will have been lowered
	if (old > 0) {
		return;
	}
	//
	// Try to trigger over outgoing arcs (non-inhibiting).  Try
	// all, but use a "funny" starting point for fairness sake.
	transref_t start = 1;  //TODO// Be "funnier"
	transref_t here = start;
	do {
		// First  pass is from start (incl) to 1     (incl)
		// Second pass is from last  (incl) to start (excl)
		trans_t trit = stat->trans_out [here];
		if (here == 0) {
			// Trigger second pass, and force check
			here = trit;
			continue;
		}
		if (get_int (pnc->topology->place_ary [trit].countdown) == 1) {
			if (try_trans (pnc, trit, plc)) {
				return;
			}
		}
		here--;
	} while (here != start);
	//
	// We did not find a place that needed just our one token, so we
	// now start to actively offer it to all.  Note that this would be
	// the first time an arc with multiplicity got triggered, so we still
	// need to incorporate fairness here.  We are thereby disadvantaging
	// such multiple arcs, which is a pitty but it falls within the reach
	// of the non-determistic Petri net properties so we shrug at it.
	// Note: here and start are set as they were for the previous loop :)
	do {
		// First  pass is from start (incl) to 1     (incl)
		// Second pass is from last  (incl) to start (excl)
		trans_t trit = stat->trans_out [here];
		if (here == 0) {
			// Trigger second pass, and force check
			here = trit;
			continue;
		}
		unsigned int old_countdown;
		do {
			old_countdown = get_int (pnc->topology->place_ary [trit].countdown);
		} while (cas_int (pnc->topology->plpace_ary [trit], old_coultdown, old_countdown-1) != old_countdown);
#ifndef BUILD_SINGLE_THREADED
		//
		// Another thread may have changed the count so we need to
		if (old_countdown == 1) {
			if (try_trans (pnc, trit, plc)) {
				return;
			}
		}
#endif
		here--;
	} while (here != start);
	//
	// Done.  We added to available and unlocked, and processed it.
	// If we processed tokens under exemption, then we should now lower
	// the token count(s) accordingly.
cleanup:
	TODO_exempted_decrement;
	return;
}


/* Unmark a place with the given number of tokens to remove.  This may be
 * used to add tokens that result from a transition firing.
 */
void unmark_place (petrinet_colour_t *pnc, placeref_t plc, tokenctr_t decr) {
	TODO;
}

/* TODO: Too much counter logic gets unfolded; have more functions! */

/* Attempt to fire a transition.  When concurrent, we need to claim all the
 * locks (while we know we have in single-threaded operation) and then start
 * the related action.  We update prior Places based on success or failure;
 * and in case of success we also update the following Places.
 *
 * The exempted place's token count will not be lowered; this can be used by
 * newly incremented tokens to avoid administrative zeal.  It should be set
 * to NIL_PLACE for all other uses.
 *
 * The return value counts the number of exempted transitions, and should be
 * used by the caller who spoke on behalf of that place, so as to correct its
 * internal administration.  Note that multiplicity of transitions does not
 * cause problems; this count is reflected in the countdown setting, so there
 * should really be enough input tokens collected for it to continue.
 */
TODO_int try_trans (petrinet_colour_t *pnc, transref_t trit, placeref_t exempt) {
	//
	// Sanity check -- does the transition want to happen?
	trans_colour_t *dyn = &pnc->trans_ary [trit];
	if (dyn->countdown > 0) {
		return TRANS_FAILURE;
	}
	time_t now = time (NULL);
	if (now < dyn->notbefore) {
#ifndef CONFIG_SINGLE_THREADED
		// Sample times and apply SECURE to deal with spurious samples
		now = dyn->notbefore - now;
		return TRANS_DEFER_SECURE (now);
#else
		return TRANS_FER (dyn->notbefore - now);
#endif
	}
	//
	// If we are running multi-threaded, in spite of all the warning flags
	// that we hung out (and not just to dry) then we should really lock
	// all the preceding places' tokens.  We assume 1 on each transition,
	// but won't mind if there are multiple arcs to the same preceding
	// place.  We only try to lock, and give up at first sight of failure.
#ifndef CONFIG_SINGLE_THREADED
	//TODO// Actually write this terrible code, complete with rollbacks
	//TODO// We should additionally lock the 0 in inhibited arcs, trouble?
#error "Code for concurrent transition prep is still incomplete"
	struct petri_t *net = pnc->topology;
	trans_t stat = &pnc->trans_ary [trit];
	trans_retcode_t rv = TRANS_SUCCESS;
	placeref_t pre;
	for (pre = stat->place_in [0]; pre > 0; pre--) {
		do {
			int old = get_int (&stat->place_in [pre]);
			if (old == 0) {
				rv = TRANS_FAILURE;
				break;
			}
		} while (cas_int (&stat->place_in [pre], old, old-1) != old);
		if (rv != TRANS_SUCCESS) {
			for (; pre <= stat->place_in [0]; pre++) {
				inc_int (&stat->place_in [pre]);
			}
			return rv;
		}
	}
#endif
	//
	// We are now sure that we can invoke the callback; process retcode
	trans_retcode_t rv = stat->TODO_action (pnc, trit, TODO_data);
	placeref_t p = NIL_PLACE;
	if (rv == SUCCESS) {
		//
		// We succeeded.  Definately remove a token from prior places.
		// While we reduce those places, they may become eligable for
		// calling.  We first reduce all places and then check for this
		// to have occurred.  Weak: we should take not of this while
		// we change the places, so we atomically know we caused it
		// and so we should initiate further action (to stick to one).
		for (p=stat->place_in [0]; p>0; p--) {
			pnc->place_ary [
				unmark_place (pnc, stat->place_in [p], 1)
				].available--;
		}
		for (p=stat->place_out [0]; p>0; p--) {
			mark_place (pnc, stat->place_out [p]);
		}
		//TODO// Find new value of countdown
	} else {
		//
		// We failed, and may have a timeout.
		if (rv != FAILURE) {
			// We got a temporary deferral
			stat->notbefore = now + rv;
		}
		//
		// Now rollback any claims we had, specifically locks.
#ifdef BUILD_SINGLE_THREADED
#error "Code for unlocking after FAILURE is not atomic yet"
		for (p=stat->place_in [0]; p>0; p--) {
			pnc->place_ary [
				unmark_place (pnc, stat->place_in [p])
				].unlocked++;
		}
#endif
		//TODO// Find new value of countdown
	}
}


