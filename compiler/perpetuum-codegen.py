#!/usr/bin/env python
#
# perpetuum-codegen.py -- Given a Petri net, generate driver code for it.
#
# This is a code generator from a set of Petri nets with suitable labels.
# The output consists of:
#  - gen tables of places and transitions, modelled after <perpetuum/model.h>
#  - use a minimum perfect hash to offer searches by key
#  - gen initialisation and, for singletons, instance information w/ marking
#  - setup initial countdown for places (TODO: incorporate inhibiting arcs)
#  - TODO: invocations of actions as declared by transition labels
#  - TODO: processing of events as declared by transition labels
#  - TODO/DROP: starting/ending of activities as declared by place labels
#  - TODO/DROP: processing of activity output as declared by place labels
#
# From: Rick van Rein <rick@openfortress.nl>


import os
import sys
import string
import random

import pntools.petrinet
import cmph


#
# Load the Petri net to work on
#

netin_fn  = os.path.dirname (sys.argv [0]) + os.sep + 'demo' + os.sep + 'netin.pnml'
netout_fn = os.path.dirname (sys.argv [0]) + os.sep + 'demo' + os.sep + 'netout.pnml'
net = pntools.petrinet.parse_pnml_file (netin_fn) [0]  #TODO# set of nets!
pntools.petrinet.write_pnml_file (net, netout_fn)
place_num = len (net.places     )
trans_num = len (net.transitions)
print 'Number of places is', place_num
print 'Number of transitions is', trans_num
print 'Network name is', net.name

#TODO# Combine Petri nets [0..] with matching labels of places and transitions


#
# Map network name to file name and structure name; produce file names
#
print 'Starting from', net.name
neat_net_name = ''.join ( [ c
			for c in net.name.replace ('-', '_').replace (' ', '_')
			if c in  '_' + string.ascii_letters + string.digits
			] )
if not neat_net_name [:1] in string.ascii_letters:
	neat_net_name = 'x' + neat_net_name
if neat_net_name == '':
	neat_net_name = 'perpetuum'
outdir = os.path.dirname (sys.argv [0]) + os.sep + 'demo' + os.sep
c_fn    = outdir + neat_net_name + '.c'
h_fn    = outdir + neat_net_name + '.h'
# dot_fn  = outdir + neat_net_name + '.dot'
# png_fn  = outdir + neat_net_name + '.dot'
pnml_fn = outdir + neat_net_name + '.pnml'
print outdir, '+', neat_net_name, '+ .pnml =', pnml_fn

#
# Write out the PNML file -- the composed result of the set of Petri nets
#
#TODO:COMPOSED#pnml = snakes.pnml.dumps (net)
#TODO:COMPOSED#print 'Produced PNML, size', len (pnml)
#TODO:COMPOSED#print 'Writing PNML to', pnml_fn
#TODO:COMPOSED#pout = open (pnml_fn, 'w')
#TODO:COMPOSED#pout.write (pnml)
#TODO:COMPOSED#pout.close ()
#TODO:COMPOSED#print 'Written', pnml_fn


#
# Map places and transitions each to their own indices
#

places = net.places     .keys ()
transs = net.transitions.keys ()

range2type = [
	( 1<< 8, 'uint8_t'  ),
	( 1<<16, 'uint16_t' ),
	( 1<<32, 'uint32_t' ),
	( 1<<64, 'uint64_t' )
]

placeref_t = None
for (r,t) in range2type:
	if len (places) < r:
		placeref_t = t
		break

transref_t = None
for (r,t) in range2type:
	if len (transs) < r:
		transref_t = t
		break

if placeref_t is None or transref_t is None:
	sys.stderr.write ('FATAL: Unable to fit places and transitions into 64 bit integers\n')
	sys.exit (1)


place_mph = cmph.generate_hash (places, algorithm='bdz')
trans_mph = cmph.generate_hash (transs, algorithm='bdz')

place_idx = { }
trans_idx = { }
place_list = [''] * place_num
trans_list = [''] * trans_num

for p in places:
	place_idx [p] = place_mph.lookup (p)
	place_list [place_idx [p]] = p
for t in transs:
	trans_idx [t] = trans_mph.lookup (t)
	trans_list [trans_idx [t]] = t

#DEBUG# print 'Places:', place_idx
#DEBUG# print 'Transitions:', trans_idx

#UNUSED# rp,wp = os.pipe ()	#PROBLEM# Limited pipe buffer
#UNUSED# place_mph.save (rp)
#UNUSED# place_hashtable = wp.read (65537)
#UNUSED# ...generate code for insertion...
#UNUSED# 
#UNUSED# rp,wp = os.pipe ()	#PROBLEM# Limited pipe buffer
#UNUSED# trans_mph.save (rp)
#UNUSED# trans_hashtable = wp.read (65537)
#UNUSED# ...generate code for insertion...


#
# Construct tables
#

cout = open (c_fn, 'w')
hout = open (h_fn, 'w')

# Generate headers for the .h and .c files
hout.write ('/* ' + neat_net_name + '''.h
 *
 * This is a generated file.  Do not edit it, but rather its source and
 * run perpetuum to produce a new version of this file.
 *
 * Please file issues with https://github.com/vanrein/perpetuum/issues
 *
 * With compliments from the ARPA2.net / InternetWide.org project!
 */


#include <stdint.h>


typedef ''' + transref_t + ''' transref_t;
typedef ''' + placeref_t + ''' placeref_t;

#include <perpetuum/model.h>


''')


cout.write ('/* ' + neat_net_name + '''.c
 *
 * This is a generated file.  Do not edit it, but rather its source and
 * run perpetuum to produce a new version of this file.
 *
 * Please file issues with https://github.com/vanrein/perpetuum/issues
 *
 * With compliments from the ARPA2.net / InternetWide.org project!
 */


#include "''' + neat_net_name + '''.h"


''')


# Generate the lists of places and transitions from each of their neighbours
def genlist (kind, dict, name, reflist):
	cout.write ('static const ' + kind + 'ref_t ' + name + ' [] = { ' + str (len (reflist)))
	for ref in reflist:
		i = 1 + dict [ref]
		cout.write (', ' + str (i))
	cout.write (' };\n')
p2t = [ (e.source,e.target) for e in net.edges if net.places.has_key (e.source) and e.type != 'inhibitor' ]
p2i = [ (e.source,e.target) for e in net.edges if net.places.has_key (e.source) and e.type == 'inhibitor' ]
t2p = [ (e.source,e.target) for e in net.edges if net.places.has_key (e.target) ]
for p in place_list:
	genlist ('trans', trans_idx, p + '_trans_out',     [ t for t in trans_list if (p,t) in p2t ] )
	genlist ('trans', trans_idx, p + '_trans_out_inh', [ t for t in trans_list if (p,t) in p2i ] )
	cout.write ('\n')
for t in trans_list:
	genlist ('place', place_idx, t + '_place_in',      [ p for p in place_list if (p,t) in p2t ] )
	genlist ('place', place_idx, t + '_place_out',     [ p for p in place_list if (t,p) in t2p ] )
	cout.write ('\n')

# Generate the place_topo_t[] from the array of each place's inputs and outputs
cout.write ('static const place_topo_t ' + neat_net_name + '_places [] = {\n')
for p in place_list:
	cout.write ('\t{ "' + p + '", ')
	cout.write (p + '_trans_out, ')
	cout.write (p + '_trans_out_inh },\n')
cout.write ('};\n\n')

cout.write ('''/* TODO: Demo mode only, this action prints transition name and timing */

#include <stdio.h>

trans_retcode_t test_action_print_trans (
				PARMDEF_COMMA (pnc)
				transref_t tr,
				time_t *nowp) {
	printf ("Firing %s -- now=%ld, notbefore=%ld, firstfail=%ld\\n",
			TRANS_NAME (pnc, tr),
			(long) *nowp,
			(long) REF2TRANS (pnc, tr).notbefore,
			(long) REF2TRANS (pnc, tr).firstfail);
	return TRANS_SUCCESS;
}

''')

# Generate the trans_topo_t[] from the array of each place's inputs and outputs
cout.write ('static const trans_topo_t ' + neat_net_name + '_transitions [] = {\n')
for t in trans_list:
	cout.write ('\t{ "' + t + '", ')
	cout.write (t + '_place_in, ')
	cout.write (t + '_place_out, ')
	cout.write ('test_action_print_trans, ')
	cout.write ('},\n')
cout.write ('};\n\n')

# Generate init vectors for places, and optional singleton array
hout.write ('/* Place initialisation */\n')
for plc in place_list:
	ini_mark = net.places [plc].marking
	hout.write ('#define PLACE_INIT_' + plc + ' { ' + str (ini_mark) + ' }\n')
hout.write ('\n')
cout.write ('#ifdef PETRINET_SINGLETONS\n')
cout.write ('static place_t the_' + neat_net_name + '_places [] = {\n')
for plc in place_list:
	cout.write ('\tPLACE_INIT_' + plc + ',\n')
cout.write ('};\n')
cout.write ('#endif\n\n')

# Generate init vectors for transitions, and optional singleton array
hout.write ('/* Place initialisation with countdown; set to inputs + non-empty inhibitors */\n')
for tr in trans_list:
	# initial "countdown" is zero normal plus non-zero inhibitors trans
	ini_countdown = ( len ( [ src for (src,tgt) in p2t
	                          if tgt == tr
	                          and net.places [src].marking == 0 ] )
			+ len ( [ src for (src,tgt) in p2i
	                          if tgt == tr
	                          and net.places [src].marking >  0 ] ) )
	hout.write ('#define TRANS_INIT_' + tr + ' { ' + str (ini_countdown) + ', 0, 0 }\n')
hout.write ('\n')
cout.write ('#ifdef PETRINET_SINGLETONS\n')
cout.write ('static trans_t the_' + neat_net_name + '_transitions [] = {\n')
for tr in trans_list:
	cout.write ('\tTRANS_INIT_' + tr + ',\n')
cout.write ('};\n')
cout.write ('#endif\n\n')

# Generate optional code for global variables, which simplifies embedded code
#TODO# Gen topology, gen transitions, gen places
hout.write ('#ifdef PETRINET_SINGLETONS\n')
hout.write ('#ifdef PETRINET_GLOBAL_NAME\n')
hout.write ('extern petrinet_t PETRINET_GLOBAL_NAME;\n')
hout.write ('#else\n')
hout.write ('extern petrinet_t the_' + neat_net_name + ';\n')
hout.write ('#endif\n')
hout.write ('#endif\n\n')
cout.write ('#ifdef PETRINET_SINGLETONS\n')
cout.write ('#ifdef PETRINET_GLOBAL_NAME\n')
cout.write ('petrinet_t PETRINET_GLOBAL_NAME = {\n')
cout.write ('#else\n')
cout.write ('petrinet_t the_' + neat_net_name + ' = {\n')
cout.write ('#endif\n')
cout.write ('\t\"the_' + neat_net_name + '",\n')
cout.write ('\t{ /* PETRINET_SINGLETONS => inlined topology */\n')
cout.write ('\t\t\"' + neat_net_name + '\",\n')
cout.write ('\t\t' + str (len (place_list)) + ',\n')
cout.write ('\t\t' + str (len (trans_list)) + ',\n')
cout.write ('\t\t&' + neat_net_name + '_places [-1],\n')
cout.write ('\t\t&' + neat_net_name + '_transitions [-1],\n')
cout.write ('\t\t/* TODO: Support for inital USRDEF_PETRINET_FIELDS */\n')
cout.write ('#ifndef PETRINET_GLOBAL_NAME\n')
cout.write ('\t},\n')
cout.write ('#else\n')
cout.write ('\t},\n')
cout.write ('#endif\n')
cout.write ('\t&the_' + neat_net_name + '_places [-1],\n')
cout.write ('\t&the_' + neat_net_name + '_transitions [-1],\n')
cout.write ('\t/* TODO: Support for initial PLACE_HASH_CTX_FIELDS */\n')
cout.write ('\t/* TODO: Support for initial TRANS_HASH_CTX_FIELDS */\n')
cout.write ('\t/* TODO: Support for initial USRDEF_PETRINET_COLOUR_FIELDS */\n')
cout.write ('};\n')
cout.write ('#endif\n\n')

# Generate the petrinet_topo_t with the static structure, based on the arrays
hout.write ('#ifndef PETRINET_SINGLETONS\n')
hout.write ('extern const petrinet_topo_t ' + neat_net_name + ';\n')
hout.write ('#else\n')
hout.write ('#ifdef PETRINET_GLOBAL_NAME\n')
hout.write ('#define ' + neat_net_name + ' (&PETRINET_GLOBAL_NAME.topology)\n')
hout.write ('#else\n')
hout.write ('#define ' + neat_net_name + ' (&the_' + neat_net_name + '->topology)\n')
hout.write ('#endif\n')
hout.write ('#endif\n\n')
cout.write ('''#ifndef PETRINET_SINGLETONS
const petrinet_topo_t ''' + neat_net_name + ''' = {
	"''' + net.name + '''",
	''' + str (place_num) + ', ' + str (trans_num) + ''',
	&''' + neat_net_name + '''_places [-1], &''' + neat_net_name + '''_transitions [-1]
};
#endif

''')

# Print an end remark
hout.write ('\n\n/* End of generated file ' + neat_net_name + '.h */\n')
cout.write ('\n\n/* End of generated file ' + neat_net_name + '.c */\n')

# Close output files
hout.close ()
cout.close ()
