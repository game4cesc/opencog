/*
 * opencog/atomspace/AtomSpaceImpl.cc
 *
 * Copyright (c) 2008-2010 OpenCog Foundation
 * Copyright (c) 2009, 2013 Linas Vepstas
 * All Rights Reserved
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License v3 as
 * published by the Free Software Foundation and including the exceptions
 * at http://opencog.org/wiki/Licenses
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program; if not, write to:
 * Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <string>
#include <iostream>
#include <fstream>
#include <list>

#include <stdlib.h>

#include <boost/bind.hpp>

#include <opencog/atomspace/ClassServer.h>
#include <opencog/atomspace/CompositeTruthValue.h>
#include <opencog/atomspace/Link.h>
#include <opencog/atomspace/Node.h>
#include <opencog/atomspace/types.h>
#include <opencog/util/Logger.h>
#include <opencog/util/oc_assert.h>

#include "AtomSpaceImpl.h"

//#define DPRINTF printf
#define DPRINTF(...)

using std::string;
using std::cerr;
using std::cout;
using std::endl;
using std::min;
using std::max;
using namespace opencog;

// ====================================================================

AtomSpaceImpl::AtomSpaceImpl(void)
    : bank(&atomTable)
{
    backing_store = NULL;

    // connect signals
    addedAtomConnection = atomTable.addAtomSignal().connect(boost::bind(&AtomSpaceImpl::atomAdded, this, _1));
    removedAtomConnection = atomTable.removeAtomSignal().connect(boost::bind(&AtomSpaceImpl::atomRemoved, this, _1));

    DPRINTF("AtomSpaceImpl::Constructor AtomTable address: %p\n", &atomTable);
}

AtomSpaceImpl::~AtomSpaceImpl()
{
    // disconnect signals
    addedAtomConnection.disconnect();
    removedAtomConnection.disconnect();
}

// ====================================================================

void AtomSpaceImpl::registerBackingStore(BackingStore *bs)
{
    backing_store = bs;
}

void AtomSpaceImpl::unregisterBackingStore(BackingStore *bs)
{
    if (bs == backing_store) backing_store = NULL;
}

// ====================================================================

void AtomSpaceImpl::atomAdded(Handle h)
{
    if (h->getType() != CONTEXT_LINK) return;

    LinkPtr lll(LinkCast(h));
    // Add corresponding VersionedTV to the contextualized atom
    // Note that when a VersionedTV is added to a
    // CompositeTruthValue it will not automatically add a
    // corresponding ContextLink
    if (lll->getArity() == 2) {
        Handle cx = lll->getOutgoingAtom(0); // context
        Handle ca = lll->getOutgoingAtom(1); // contextualized atom
        ca->setTV(lll->getTV(), VersionHandle(CONTEXTUAL, cx));
    } else
        throw RuntimeException(TRACE_INFO,
            "AtomSpaceImpl::atomAdded: Invalid arity for a ContextLink: %d (expected: 2)\n", 
        lll->getArity());
}

void AtomSpaceImpl::atomRemoved(AtomPtr atom)
{
    if (atom->getType() != CONTEXT_LINK) return;

    // Remove corresponding VersionedTV to the contextualized atom
    // Note that when a VersionedTV is removed from a
    // CompositeTruthValue it will not automatically remove the
    // corresponding ContextLink
    LinkPtr lll(LinkCast(atom));
    OC_ASSERT(lll->getArity() == 2,
        "AtomSpaceImpl::atomRemoved: Got invalid arity for removed ContextLink = %d\n",
        lll->getArity());
    Handle cx = lll->getOutgoingAtom(0); // context
    Handle ca = lll->getOutgoingAtom(1); // contextualized atom
    TruthValuePtr tv = ca->getTV();
    CompositeTruthValuePtr new_ctv(CompositeTruthValue::createCTV(tv));
    new_ctv->removeVersionedTV(VersionHandle(CONTEXTUAL, cx));
    // @todo: one may want improve that code by converting back
    // the CompositeTV into a simple or indefinite TV when it has
    // no more VersionedTV
    ca->setTV(new_ctv);
}

// ====================================================================

AtomSpaceImpl& AtomSpaceImpl::operator=(const AtomSpaceImpl& other)
{
    throw opencog::RuntimeException(TRACE_INFO,
            "AtomSpaceImpl - Cannot copy an object of this class");
}

AtomSpaceImpl::AtomSpaceImpl(const AtomSpaceImpl& other)
    : bank(&atomTable)
{
    throw opencog::RuntimeException(TRACE_INFO,
            "AtomSpaceImpl - Cannot copy an object of this class");
}

Handle AtomSpaceImpl::addNode(Type t, const string& name, TruthValuePtr tvn)
{
    // Is this atom already in the atom table? 
    Handle hexist = atomTable.getHandle(t, name);
    if (hexist)
    {
        // XXX FIXME The AtomSpaceAsyncUTest tries to do somethig weird,
        // that is causing the below to fail. The atomtable add does the
        // same thing under a lock. I don't understand why it matters.
        // It would be slight faster to just merge here, but wtf ... 
        // So anyway, ifdef it out, for now ...
#ifdef DO_UNLOCKED_UPDATE
        hexist->merge(tvn);  // Update the truth value.
        return hexist;
#else
        return atomTable.add(createNode(t, name, tvn));
#endif
    }

    // If we are here, the AtomTable does not yet know about this atom.
    // Maybe the backing store knows about this atom.
    if (backing_store and not backing_store->ignoreType(t)) {

        NodePtr n(backing_store->getNode(t, name.c_str()));
        if (n) {
            Handle result = atomTable.add(n);
            result->merge(tvn);
            return result;
        }
    }

    // If we are here, neither the AtomTable nor backing store know about
    // this atom. Just add it.
    return atomTable.add(createNode(t, name, tvn));
}

Handle AtomSpaceImpl::getNode(Type t, const string& name)
{
    // Is this atom already in the atom table? 
    Handle hexist = atomTable.getHandle(t, name);
    if (hexist) return hexist;

    // If we are here, the AtomTable does not yet know about this atom.
    // Maybe the backing store knows about this atom.
    if (backing_store and not backing_store->ignoreType(t))
    {
        NodePtr n(backing_store->getNode(t, name.c_str()));
        if (n) {
            return atomTable.add(n);
        }
    }

    // If we are here, nobody knows about this.
    return Handle::UNDEFINED;
}

Handle AtomSpaceImpl::addLink(Type t, const HandleSeq& outgoing,
                              TruthValuePtr tvn)
{
    // Is this atom already in the atom table?
    Handle hexist = atomTable.getHandle(t, outgoing);
    if (hexist)
    {
        hexist->merge(tvn);  // Update the truth value.
        return hexist;
    }

    // If we are here, the AtomTable does not yet know about this atom.
    // Maybe the backing store knows about this atom.
    if (backing_store and not backing_store->ignoreType(t))
    {
        // If any of the outgoing set is ignorable, we will not
        // fetch the thing from the backing store.
        if (not std::any_of(outgoing.begin(), outgoing.end(),
            [this](Handle ho) { return backing_store->ignoreAtom(ho); }))
        {
            LinkPtr l(backing_store->getLink(t, outgoing));
            if (l) {
                // Put the atom into the atomtable, so it gets placed
                // in indices, so we can find it quickly next time.
                Handle result = atomTable.add(l);
                result->merge(tvn);
                return result;
            }
        }
    }

    // If we are here, neither the AtomTable nor backing store know about
    // this atom. Just add it.
    return atomTable.add(createLink(t, outgoing, tvn));
}

Handle AtomSpaceImpl::getLink(Type t, const HandleSeq& outgoing)
{
    // Is this atom already in the atom table?
    Handle hexist = atomTable.getHandle(t, outgoing);
    if (hexist) return hexist;

    // If we are here, the AtomTable does not yet know about this atom.
    // Maybe the backing store knows about this atom.
    if (backing_store and not backing_store->ignoreType(t))
    {
        // If any of the outgoing set is ignorable, we will not
        // fetch the thing from the backing store.
        if (not std::any_of(outgoing.begin(), outgoing.end(),
            [this](Handle ho) { return backing_store->ignoreAtom(ho); }))
        {
            LinkPtr l(backing_store->getLink(t, outgoing));
            if (l) {
                // Register the atom with the atomtable (so it gets placed in
                // indices)
                return atomTable.add(l);
            }
        }
    }

    // If we are here, nobody knows about this.
    return Handle::UNDEFINED;
}

void AtomSpaceImpl::storeAtom(Handle h)
{
    if (backing_store) backing_store->storeAtom(h);
}

Handle AtomSpaceImpl::fetchAtom(Handle h)
{
    if (NULL == backing_store)
        return Handle::UNDEFINED;

    // If the atom corresponding to the UUID isn't available, then
    // got get it. But in fact, we may already have it, from a
    // previous recusrive call to getIncomingSet(), so don't waste
    // any CPU sycles getting it again.
    if (NULL == h.operator->())
        h = backing_store->getAtom(h);

    // For links, must perform a recursive fetch, as otherwise
    // the atomtable.add below will throw an error.
    LinkPtr l(LinkCast(h));
    if (l) {
       const HandleSeq& ogs = l->getOutgoingSet();
       size_t arity = ogs.size();
       for (size_t i=0; i<arity; i++)
       {
          Handle oh = fetchAtom(ogs[i]);
          if (oh != ogs[i]) throw RuntimeException(TRACE_INFO,
              "Unexpected handle mismatch! Expected %lu got %lu\n",
              ogs[i].value(), oh.value());
       }
    }
    if (h) return atomTable.add(h);

    return Handle::UNDEFINED;
}

Handle AtomSpaceImpl::getAtom(Handle h)
{
    if (atomTable.holds(h)) return h;
    return fetchAtom(h);
}

Handle AtomSpaceImpl::fetchIncomingSet(Handle h, bool recursive)
{
    h = getAtom(h);

    if (Handle::UNDEFINED == h) return Handle::UNDEFINED;

    // Get everything from the backing store.
    if (backing_store) {
        HandleSeq iset = backing_store->getIncomingSet(h);
        size_t isz = iset.size();
        for (size_t i=0; i<isz; i++) {
            Handle hi = iset[i];
            if (recursive) {
                fetchIncomingSet(hi, true);
            } else {
                getAtom(hi);
            }
        }
    }
    return h;
}

HandleSeq AtomSpaceImpl::getNeighbors(Handle h, bool fanin,
        bool fanout, Type desiredLinkType, bool subClasses) const
{
    if (h == NULL) {
        throw InvalidParamException(TRACE_INFO,
            "Handle %d doesn't refer to a Atom", h.value());
    }
    HandleSeq answer;

    HandleSeq iset = atomTable.getIncomingSet(h);
    for (HandleSeq::iterator it = iset.begin();
         it != iset.end(); it++)
    {
        LinkPtr link(LinkCast(*it));
        Type linkType = link->getType();
        DPRINTF("Atom::getNeighbors(): linkType = %d desiredLinkType = %d\n", linkType, desiredLinkType);
        if ((linkType == desiredLinkType) || (subClasses && classserver().isA(linkType, desiredLinkType))) {
            Arity linkArity = link->getArity();
            for (Arity i = 0; i < linkArity; i++) {
                Handle handle(link->getOutgoingSet()[i]);
                if (handle == h) continue;
                if (!fanout && link->isSource(h)) continue;
                if (!fanin && link->isTarget(h)) continue;
                answer.push_back(handle);
            }
        }
    }
    return answer;
}

HandleSeq AtomSpaceImpl::getIncoming(Handle h)
{
    // It is possible that the incoming set that we currently hold is
    // much smaller than what is in persistant storage. In this case,
    // we would like to automatically pull all of those other atoms
    // into here (using fetchIncomingSet(h,true) to do so). However,
    // maybe the incoming set is up-to-date, in which case polling
    // storage over and over is a huge waste of time.  What to do?
    //
    // h = fetchIncomingSet(h, true);
    //
    // Anyway, the user can call fetch explicitly, if desired.

    return atomTable.getIncomingSet(h);
}

bool AtomSpaceImpl::deleteAtom(Handle h, bool recursive)
{
    if (backing_store) {
// Under construction .... 
        throw RuntimeException(TRACE_INFO, "Not Implemented!!!");
    }
    return 0 < atomTable.extract(h, recursive).size();
}

void AtomSpaceImpl::clear()
{
    std::vector<Handle> allAtoms;

    atomTable.getHandlesByType(back_inserter(allAtoms), ATOM, true);

    DPRINTF("%d nodes %d links to erase\n", Nodes(NULL_VERSION_HANDLE),
            Links(NULL_VERSION_HANDLE));
    DPRINTF("atoms in allAtoms: %lu\n", allAtoms.size());

    Logger::Level save = logger().getLevel();
    logger().setLevel(Logger::DEBUG);

    // XXX FIXME TODO This is a stunningly inefficient way to clear the
    // atomspace! This will take minutes on any decent-sized atomspace!
    size_t j = 0;
    std::vector<Handle>::iterator i;
    for (i = allAtoms.begin(); i != allAtoms.end(); ++i) {
        bool result = removeAtom(*i, true);
        if (result) {
            DPRINTF("%d: Atom %lu removed, %d nodes %d links left to delete\n",
                j, i->value(), Nodes(NULL_VERSION_HANDLE), Links(NULL_VERSION_HANDLE));
            j++;
        }
    }

    allAtoms.clear();
    atomTable.getHandlesByType(back_inserter(allAtoms), ATOM, true);
    assert(allAtoms.size() == 0);

    logger().setLevel(save);
}

