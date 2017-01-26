/*
  GUIDO Library
  Copyright (C) 2002  Holger Hoos, Juergen Kilian, Kai Renz
  Copyright (C) 2002-2013 Grame

  This Source Code Form is subject to the terms of the Mozilla Public
  License, v. 2.0. If a copy of the MPL was not distributed with this
  file, You can obtain one at http://mozilla.org/MPL/2.0/.

  Grame Research Laboratory, 11, cours de Verdun Gensoul 69002 Lyon - France
  research@grame.fr

*/

#include "ARShareStem.h"
#include "ARTStem.h"
#include "ARDisplayDuration.h"
#include "ARNoteFormat.h"

#include "TagParameterString.h"
#include "TagParameterFloat.h"

#include "GRGlobalStem.h"

#include "GRStem.h"
#include "GRStaff.h"
#include "GREmpty.h"
#include "GRSpring.h"
#include "GRFlag.h"
#include "GRSingleNote.h"
#include "GRSystem.h"
#include "GRSystemTag.h"
#include "GRStdNoteHead.h"
#include "GRVoice.h"
#include "GRSystemSlice.h"
#include "GRNoteDot.h"
#include "GRAccidental.h"


GRGlobalStem::GRGlobalStem( GRStaff * inStaff, ARShareStem * pshare, ARTStem * stemstate, ARDisplayDuration * dispdur, ARNoteFormat * noteformat )
	: GRPTagARNotationElement(pshare), fFlagOnOff(true), fStemdirSet(false), fStemlengthSet(false),
	fStemdir(dirOFF), fFirstEl(NULL), fLowerNote(NULL), fHigherNote(NULL)
{
	if (dispdur && dispdur->getDisplayDuration() > DURATION_0)
		fDispdur = dispdur->getDisplayDuration();

	GRSystemStartEndStruct * sse = new GRSystemStartEndStruct;

	sse->grsystem = inStaff->getGRSystem();
	sse->startElement = NULL;
	sse->startflag = GRSystemStartEndStruct::LEFTMOST;
	sse->endElement = NULL;
	sse->p = NULL;

	mStartEndList.AddTail(sse);

	fStemState = stemstate;
	fStem = NULL;
	fFlag = NULL;
	fStemlengthSet = false;

    fStaffSize = mTagSize = inStaff->getSizeRatio();
	if (noteformat) {
		const TagParameterFloat * tmp = noteformat->getSize();
		if (tmp) mTagSize = tmp->getValue();

		// color ...
		const TagParameterString * tmps = noteformat->getColor();
		if (tmps) {
			if (!mColRef) mColRef = new unsigned char[ 4 ];
			tmps->getRGB(mColRef);
		}

		// offset ....
		const TagParameterFloat * tmpdx = noteformat->getDX();
		const TagParameterFloat * tmpdy = noteformat->getDY();
		if (tmpdx)
			mTagOffset.x = (GCoord)(tmpdx->getValue(inStaff->getStaffLSPACE()));

		if (tmpdy) {
			mTagOffset.y = (GCoord)(tmpdy->getValue(inStaff->getStaffLSPACE()));
            mTagOffset.y = -mTagOffset.y;
		}
	}
}

GRGlobalStem::~GRGlobalStem()
{
	// we just remove any association manually
	if (mAssociated)
	{
		GuidoPos pos = mAssociated->GetHeadPosition();
		while(pos)
		{
			GRNotationElement * el = mAssociated->GetNext(pos);
			if( el ) el->removeAssociation(this);
		}
	}
	if (fFirstEl)
		fFirstEl->removeAssociation(this);

	delete fStem; fStem = 0;
	delete fFlag; fFlag = 0;
//	if (colref)	delete [] colref;
}

void GRGlobalStem::addAssociation(GRNotationElement * grnot)
{
	if (error) return;
	
	GREvent * ev = GREvent::cast(grnot);
	if( ev ) {
		TYPE_DURATION evdur (ev->getDuration());
		TYPE_DURATION durtempl;
		GRSingleNote * sngnot = dynamic_cast<GRSingleNote *>(ev);
		if (sngnot)
			durtempl       = sngnot->getDurTemplate();
		
		// this changes the display-duration (nested display-duration-tags!)
		if (durtempl>evdur && durtempl> fDispdur)
			fDispdur = durtempl;

		// take the length ...
		if (evdur > DURATION_0) {
			if (fFirstEl) {
				// then we already have one duration event. In this case, the association is added because
				// another voice uses this globalstem because of a \chord<label>-Tag
				ev->setGlobalStem(this);
				ev->setNeedsSpring(1);
			}
			else {
				// then our springconstant changes ...
				ev->setGlobalStem(this);
				if (!fFirstEl) ev->setNeedsSpring(1);
			}
		}
		else {
			ev->setGlobalStem(this);
			if (!fFirstEl) ev->setNeedsSpring(1);
		}
	}

	// This sets the first element in the range ...
	if (fFirstEl == 0) {
		// this associates the first element with this tag ....
		fFirstEl = grnot;
		fFirstEl->addAssociation(this);
		return;

		// the firstElement is not added to the associated
		// ones -> it does not have to told anything!?
	}

	GRPTagARNotationElement::addAssociation(grnot);

	// this is needed, because the share location can be used in different staves.
	// Elements that need be told of the location can be deleted before this tag,
	// Therefore we must know, if these elements are deleted.
	// The recursive cycle with tellPosition is broken, because only the first element really sets the
	// position. And the first is not included in the own mAssociated list.
	grnot->addAssociation(this);
}

void GRGlobalStem::removeAssociation(GRNotationElement * grnot)
{
    if (grnot == fFirstEl) {
		GRNotationElement::removeAssociation(grnot);
		fFirstEl = 0;
	}
	else
		GRPTagARNotationElement::removeAssociation(grnot);
}

GDirection GRGlobalStem::getStemDir() const
{
	if (fStem == 0) return dirOFF;
	return fStem->getStemDir();
}

void GRGlobalStem::RangeEnd( GRStaff * inStaff)
{
	if (error || fFirstEl == 0) return;

	GRPTagARNotationElement::RangeEnd(inStaff);
	if (inStaff == 0) return;

	GRSystemStartEndStruct * sse = getSystemStartEndStruct(inStaff->getGRSystem());

	// this checks, wether all associated elements are on the same staff. If not, we build a new
	// GRSystemTag that gets added to the system so that an update on all positions can be made ....
	// (this is taken from GRBeam)
	GuidoPos syststpos = sse->startpos;
	if (tagtype != GRTag::SYSTEMTAG && syststpos)
	{
		// this is all done so that I really get a correct first staff to test my stuff ...
		while (syststpos && !(mAssociated->GetAt(syststpos)))
			mAssociated->GetNext(syststpos);

		int tststaffnum = mAssociated->GetNext(syststpos)->getStaffNumber();
		while (syststpos) {
			GRNotationElement * el = mAssociated->GetNext(syststpos);
			if (el) {
				if (el->getStaffNumber() != tststaffnum) {
					tagtype = GRTag::SYSTEMTAG;
					GRSystemTag * mysystag = new GRSystemTag(this);
					el->getGRSystemSlice()->addSystemTag(mysystag);
					break;
				}
			}		
		}
	}

	if (tagtype != GRTag::SYSTEMTAG) {
		// check, whether firstel is on the same staff?
		if (fFirstEl && mAssociated && mAssociated->GetHead()) {
			if (fFirstEl->getStaffNumber() != mAssociated->GetHead()->getStaffNumber() ) {
				tagtype = GRTag::SYSTEMTAG;
				GRSystemTag * mysystag = new GRSystemTag(this);
				fFirstEl->getGRSystemSlice()->addSystemTag(mysystag);
			}
		}
	}

	
	GRNotationElement * el = /*dynamic cast<GRNotationElement *>*/(this);
	const NEPointerList * associated = el ? el->getAssociations() : 0;
	if (associated == 0)  return;

	// now I have the associations ... I have to build the stem ....
	delete fStem;

	fStem = new GRStem(this);
	if (mColRef)
		fStem->setColRef( mColRef );

	// the Vertical position of the Notes that share a stem must be already set ....
	int highestlowestset = 0;
	// only if the stemdir has not been set ....
	if (fStemdir == dirOFF) {
		fStemdir = dirUP;
		if (fStemState) {
			if (fStemState->getStemState() == ARTStem::UP)
				fStemdir = dirUP;		// we have to determine the direction ourselves.
			else if (fStemState->getStemState() == ARTStem::DOWN)
				fStemdir = dirDOWN;		// we have to determine the direction ourselves.
			else if (fStemState->getStemState() == ARTStem::OFF)
				fStemdir = dirOFF;		// we have to determine the direction ourselves.
		}

		if ( ( fStemState && fStemState->getStemState() == ARTStem::AUTO  ) || !fStemState)
		{			
			// we have to determine the direction ourselves.
			// this needs to be done with the direction of notes ...
			GCoord middle = 0;
			int count = 0;
			// determine the lowest and highest position ...
			el = associated->GetTail();
			if (el) {
				middle = el->getPosition().y;
				if (tagtype == GRTag::SYSTEMTAG && el->getGRStaff())
					middle += (GCoord)el->getGRStaff()->getPosition().y;
				fHighestY = middle;
				fLowestY = middle;
				++ count;

                if (dynamic_cast<GRSingleNote *>(el))
				    fHigherNote = fLowerNote = dynamic_cast<GRSingleNote *>(el);
			}
			
			GuidoPos pos = associated->GetHeadPosition();
			while (pos && pos != associated->GetTailPosition()) {
				GRNotationElement * el = associated->GetNext(pos);
				if (el && !dynamic_cast<GREmpty *>(el)) {
					GCoord ypos = el->getPosition().y;
					if (el->getGRStaff() && tagtype == GRTag::SYSTEMTAG)
						ypos += el->getGRStaff()->getPosition().y;
					middle += ypos;
					++count ;
					
					if (fLowestY > ypos) {
						fLowestY = ypos;
						fLowerNote = (GRSingleNote *)el;
					}
					if (fHighestY < ypos) {
						fHighestY = ypos;
						fHigherNote = (GRSingleNote *)el;
					}
				}
			}
			
			highestlowestset = 1;
			if (count > 0) middle /= count;
			
			const float curLSPACE = (float)(inStaff->getStaffLSPACE());
			const float mylowesty = 2 * curLSPACE - fLowestY;
			const float myhighesty = fHighestY - 2 * curLSPACE;
			
			if (mylowesty > myhighesty)			fStemdir = dirDOWN;
			else if (myhighesty > mylowesty)	fStemdir = dirUP;
			else {
				if (middle >= curLSPACE * 2)	 fStemdir = dirUP;
				else if (middle < curLSPACE * 2) fStemdir = dirDOWN;
			}
		}
		else // - If stem's direction is fixed by the user, we have to determine lower and higher chord note all the same
		{
			GCoord middle = 0;
			int count = 0;
			el = associated->GetTail();

			if (el)
			{
				middle = el->getPosition().y;
				if (tagtype == GRTag::SYSTEMTAG && el->getGRStaff())
				{
					middle += (GCoord)el->getGRStaff()->getPosition().y;
				}
				fHighestY = middle;
				fLowestY = middle;
				++ count;

				fLowerNote  = (GRSingleNote *)el;
				fHigherNote = (GRSingleNote *)el;
			}

			GuidoPos pos = associated->GetHeadPosition();
			while (pos && pos != associated->GetTailPosition())
			{
				GRNotationElement * el = associated->GetNext(pos);
				if (el && !dynamic_cast<GREmpty *>(el))
				{
					GCoord ypos = el->getPosition().y;
					if (el->getGRStaff() && tagtype == GRTag::SYSTEMTAG)
						ypos += el->getGRStaff()->getPosition().y;

					if (fLowestY > ypos)
					{
						fLowestY = ypos;
						fLowerNote = (GRSingleNote *)el;
					}
					if (fHighestY < ypos)
					{
						fHighestY = ypos;
						fHigherNote = (GRSingleNote *)el;
					}
				}
			}
		}
	}
	
    if (fDispdur >= DURATION_1)
		fStemdir = dirOFF;
	fStem->setStemDir(fStemdir);
	

	// otherwise it has been set because of auto-stem.
	if (!highestlowestset)
	{
		// determine the lowest and highest position ...
		el = associated->GetTail();
		if (el) {
			fLowestY = el->getPosition().y;
			if (tagtype == GRTag::SYSTEMTAG && el->getGRStaff())
				fLowestY += el->getGRStaff()->getPosition().y;
			fHighestY = fLowestY;
		}
		
		GuidoPos pos = associated->GetHeadPosition();
		while (pos) {
			GRNotationElement * el = associated->GetNext(pos);
			if (el && !dynamic_cast<GREmpty *>(el)) {
				NVPoint elpos (el->getPosition());
				if (tagtype == GRTag::SYSTEMTAG && el->getGRStaff())
					elpos += el->getGRStaff()->getPosition();
				
				if (fLowestY > elpos.y) 	fLowestY = elpos.y;
				if (fHighestY < elpos.y)	fHighestY = elpos.y;
			}
		}
		
	}

	// now we have the position of the lowest or highest note ...
	if (fStemdir == dirUP)
		fStem->setPosition(NVPoint(0, (GCoord)fHighestY));
	else if (fStemdir == dirDOWN)
		fStem->setPosition(NVPoint(0, (GCoord)fLowestY));

	// now we have to deal with the length...
	const TagParameterFloat * taglength = 0;
	if (fStemState)
		taglength = fStemState->getLength();
	if (fStemState && taglength  && taglength->TagIsSet()) {
		// we have a length, that was definitly set...
		fStem->setStemLength((float)(fStemState->getLength()->getValue()));
		fStemlengthSet = true;
	}
	else {
		// length was not set ....
        float length = fHighestY - fLowestY + inStaff->getStaffLSPACE() * 3.5f * mTagSize / fStaffSize;
//		bool hastrill = false;
//		GuidoPos pos = associated->GetHeadPosition();
//		while (pos) {
//			GRNotationElement * el = associated->GetNext(pos);
//			const GRSingleNote* note = el->isSingleNote();
//			if (note && note->hasTrill()) {
//				length = (note->getDirection() != dirDOWN) ? 0 : length;
//				break;
//			}
//		}
//        float length = fHighestY - fLowestY + inStaff->getStaffLSPACE() * 3.5f * mTagSize / fStaffSize;
//        float length = hastrill ? 0 : (float)(fHighestY - fLowestY + inStaff->getStaffLSPACE() * 3.5f * mTagSize / fStaffSize);
		fStem->setStemLength( length );
	}
	delete fFlag;

	// here we have to add the flags ...
	fFlag = new GRFlag(this, fDispdur, fStemdir, fStem->getStemLength());
	if (mColRef)					fFlag->setColRef(mColRef);
	if (!fFlagOnOff)				fFlag->setFlagOnOff(fFlagOnOff);
	if (fStemdir == dirUP)			fFlag->setPosition(NVPoint(0, (GCoord)fHighestY));
	else if (fStemdir == dirDOWN)	fFlag->setPosition(NVPoint(0, (GCoord)fLowestY));

	if (tagtype != GRTag::SYSTEMTAG)
		updateGlobalStem(inStaff);

	const float curLSPACEtmp = (float)(inStaff->getStaffLSPACE());
	if (fStemdir == dirUP) {
		NVPoint stemendpos (fStem->getPosition());
		stemendpos.y -= fStem->getStemLength();
        float coef = 0;

        if ((float) fDispdur.getNumerator() / (float) fDispdur.getDenominator() <= 1.0f / 64.0f && fHighestY > 250)
            coef = 0;
        else if ((float) fDispdur.getNumerator() / (float) fDispdur.getDenominator() <= 1.0f / 32.0f && fHighestY > 250)
            coef = 1;
        else
            coef = 0.5f * inStaff->getNumlines() - 0.5f; // Stem length is set everytime as far as the middle of the staff.

        if (stemendpos.y > coef * curLSPACEtmp) {
            const float newlength = (fStem->getPosition().y - coef * curLSPACEtmp);
            float lengthDiff = newlength - getStemLength();
            changeStemLength(newlength);

            fFlag->setPosition( NVPoint(fFlag->getPosition().x, fFlag->getPosition(). y - lengthDiff));
        }
	}
	else if (fStemdir == dirDOWN) {
		NVPoint stemendpos (fStem->getPosition());
		stemendpos.y += fStem->getStemLength();
        float coef = 0;

        if ((float) fDispdur.getNumerator() / (float) fDispdur.getDenominator() <= 1.0f / 64.0f && fLowestY < - 50)
            coef = 4;
        else if ((float) fDispdur.getNumerator() / (float) fDispdur.getDenominator() <= 1.0f / 32.0f && fLowestY < - 50)
            coef = 3;
        else
            coef = 0.5f * inStaff->getNumlines() - 0.5f; // Stem length is set everytime as far as the middle of the staff.

        if (stemendpos.y < coef * curLSPACEtmp) {
            const float newlength = (coef * curLSPACEtmp - fStem->getPosition().y);
            float lengthDiff = newlength - getStemLength();
            changeStemLength(newlength);

            fFlag->setPosition( NVPoint(fFlag->getPosition().x, fFlag->getPosition().y + lengthDiff));
        }
	}
}


void GRGlobalStem::updateGlobalStem(const GRStaff * inStaff)
{
	// now we can adjust the headoffsets for the notes ...
	// maybe we should sort the mAssociated list by y-position.
	// then we can travers the noteheads an make the adjustment (semi-)automatic.
	mAssociated->sort( &compnotposy );

	const float curLSPACE = inStaff->getStaffLSPACE();

	ARTHead::HEADSTATE sugHeadState;
	ARTHead::HEADSTATE prevHeadState = ARTHead::NOTSET;
	float prevposy = 0;	// (JB) warning, was not initialized. I don't know if 0 is a good init value ! TODO: verify.

	// the first element is in most cases the empty-event!?
	GRSingleNote * note = dynamic_cast<GRSingleNote *>(fFirstEl);
	if (note) {
		note->adjustHeadPosition(ARTHead::NORMAL);
		note->updateBoundingBox();
	}

    // Variables for dot's offset...
    // ...horizontally
    float offsetMax = 0;
    NVPoint currentNoteHeadOffset = NVPoint(0, 0);
    bool differentOffsets = false;
    bool prevOffsetExisting = false;
    float prevOffset = 0;
    // ...vertically
    std::vector<float> yOffsetVector;

	if (fStemdir == dirDOWN)
	{
		sugHeadState = ARTHead::RIGHT;
        float minXHeadOffset = 0;
		GuidoPos pos = mAssociated->GetHeadPosition();
		while (pos)
		{
			note = dynamic_cast<GRSingleNote *>(mAssociated->GetNext(pos));
			if (note)
			{
				if (prevHeadState != ARTHead::NOTSET)
				{
					float cury = (float)note->getPosition().y;
					if (tagtype == GRTag::SYSTEMTAG)
						cury += (float)note->getGRStaff()->getPosition().y;

                    const float tmpCurLSPACE = (curLSPACE - curLSPACE / 50); // To avoid precision problems
					// y-values are ascending.
					if (cury != prevposy && cury - prevposy < tmpCurLSPACE && !note->getGRCluster())
					{
						// then I have to reverse the headsuggestion.
                        if (prevHeadState == ARTHead::RIGHT)
							sugHeadState = ARTHead::LEFT;
						else
							sugHeadState = ARTHead::RIGHT;
					}
					else if (cury - prevposy >= curLSPACE)
					{
						if (prevHeadState == ARTHead::LEFT)
							sugHeadState = ARTHead::RIGHT;
					}
				}
				ARTHead::HEADSTATE retHeadState = note->adjustHeadPosition(sugHeadState);
				// now we have a current headstate ....

                /* To adjust the dot's offset... */
                if (note->getNoteHead())
                {
                    /* ...horizontally */
                    currentNoteHeadOffset = note->getNoteHead()->getOffset();

                    // Keeping track of the minimum offset
                    if(currentNoteHeadOffset.x<minXHeadOffset)
                        minXHeadOffset = currentNoteHeadOffset.x;

                    if (offsetMax < currentNoteHeadOffset.x)
                        offsetMax = currentNoteHeadOffset.x;

                    if (prevOffsetExisting) {
                        if (prevOffset != currentNoteHeadOffset.x)
                            differentOffsets = true;
                    }
                    else {
                        prevOffset = currentNoteHeadOffset.x;
                        prevOffsetExisting = true;
                    }

                    /* ...vertically */
                    if (note->getGRCluster() == NULL && note->getDot())
                    {
                        float dotPosition = note->getDot()->getPosition().y + note->getDot()->getOffset().y;

                        if (!yOffsetVector.size())
                            yOffsetVector.push_back(dotPosition);
                        else {
                            bool found = false;

                            size_t vectorSize = yOffsetVector.size();
                            for (size_t i = 0; i < vectorSize; i++)
                            {
                                if (yOffsetVector[i] - 10 < dotPosition && yOffsetVector[i] + 10 > dotPosition)
                                {
                                    bool withOffsetFound = false;

                                    for (size_t j = 0; j < vectorSize; j++)
                                    {
                                        if (dotPosition + LSPACE - 10 < yOffsetVector[j] && dotPosition + LSPACE + 10 > yOffsetVector[j])
                                            withOffsetFound = true;
                                    }

                                    if (!withOffsetFound)
                                    {
                                        note->getDot()->addOffsetY(LSPACE);
                                        yOffsetVector.push_back(dotPosition + LSPACE);
                                        found = true;
                                        break;
                                    }
                                    else {
                                        withOffsetFound = false;

                                        for (size_t j = 0; j < vectorSize; j++) {
                                            if (dotPosition - LSPACE - 10 < yOffsetVector[j] && dotPosition - LSPACE + 10 > yOffsetVector[j])
                                                withOffsetFound = true;
                                        }

                                        if (!withOffsetFound) {
                                            note->getDot()->addOffsetY(-LSPACE);
                                            yOffsetVector.push_back(dotPosition - LSPACE);
                                            found = true;
                                            break;
                                        }
                                    }
                                }
                            }
                            if (!found) yOffsetVector.push_back(dotPosition);
                        }
                    }
                }

				prevHeadState = retHeadState;
				prevposy = note->getPosition().y;
				if (tagtype == GRTag::SYSTEMTAG)
					prevposy += note->getGRStaff()->getPosition().y;

				note->updateBoundingBox();
            }
        }

		// - Adjust stem length if it's a cross/triangle notehead
		if (fLowerNote)
		{
			GRStdNoteHead* head = fLowerNote->getNoteHead();
			unsigned char lowerNoteSymbol = kNoneSymbol;
			if (head && !fLowerNote->isEmpty()) lowerNoteSymbol = head->getSymbol();

			if (lowerNoteSymbol == kFullXHeadSymbol)
			{
				fLowerNote->setFirstSegmentDrawingState(false);
				fLowerNote->setStemOffsetStartPosition(-4);
			}
			else if (lowerNoteSymbol == kFullTriangleHeadSymbol || lowerNoteSymbol == kHalfTriangleHeadSymbol)
				fLowerNote->setFirstSegmentDrawingState(false);
			else if (lowerNoteSymbol == kFullReversedTriangleHeadSymbol || lowerNoteSymbol == kHalfReversedTriangleHeadSymbol)
				fHigherNote->setStemOffsetStartPosition(-47);
		}

        // - Set notehead orientation for extreme chord note
        if (fLowerNote)
            fStem->setLastHeadOrientation (fLowerNote->getHeadState());

        // - Adjust accidentals offset the minimal X head offset is less than 0
        if(minXHeadOffset){
            NVPoint offsetToAdd( minXHeadOffset,0);

            GuidoPos pos = mAssociated->GetHeadPosition();
            while (pos)
            {
                note = dynamic_cast<GRSingleNote *>(mAssociated->GetNext(pos));
                if (note){
                    GRAccidentalList accidentalList;
                    note->extractAccidentals(&accidentalList);
                    GuidoPos accPos = accidentalList.GetHeadPosition();
                    while(accPos){
                        GRAccidental* accidental = accidentalList.GetNext(accPos);
                        accidental->addToOffset(offsetToAdd);
                    }
                    note->updateBoundingBox();
                }
            }
        }

	}
	else if (fStemdir == dirUP || fStemdir == dirOFF)
	{
		sugHeadState = ARTHead::LEFT;
		GuidoPos pos = mAssociated->GetTailPosition();
		while (pos)
		{
			note = dynamic_cast<GRSingleNote *>(mAssociated->GetPrev(pos));
			if (note)
			{
				if (prevHeadState != ARTHead::NOTSET)
				{
					float cury = (float)note->getPosition().y;
					if (tagtype == GRTag::SYSTEMTAG)
						cury += note->getGRStaff()->getPosition().y;

                    const float tmpCurLSPACE = (curLSPACE - curLSPACE / 50); // To avoid precision problems
					// y-values are decending.
					if (cury != prevposy && prevposy - cury < tmpCurLSPACE && !note->getGRCluster())
					{
						// then I have to reverse the headsuggestion.
						if (prevHeadState == ARTHead::RIGHT)
							sugHeadState = ARTHead::LEFT;
						else
							sugHeadState = ARTHead::RIGHT;
					}
					else if (prevposy - cury  >= curLSPACE)
					{
						if (prevHeadState == ARTHead::RIGHT)
							sugHeadState = ARTHead::LEFT;
					}

				}
				ARTHead::HEADSTATE retHeadState = note->adjustHeadPosition(sugHeadState);
                // now we have a current headstate ....

                /* To adjust the horizontal dot's offset */
                if (note->getNoteHead())
                {
                    currentNoteHeadOffset = note->getNoteHead()->getOffset();

                    if (offsetMax < currentNoteHeadOffset.x)
                        offsetMax = currentNoteHeadOffset.x;

                    if (prevOffsetExisting)
                    {
                        if (prevOffset != currentNoteHeadOffset.x)
                            differentOffsets = true;
                    }
                    else
                    {
                        prevOffset = currentNoteHeadOffset.x;
                        prevOffsetExisting = true;
                    }


                    /* ...vertically */
                    if (note->getGRCluster() == NULL && note->getDot())
                    {
                        float dotPosition = note->getDot()->getPosition().y + note->getDot()->getOffset().y;

                        if (!yOffsetVector.size())
                            yOffsetVector.push_back(dotPosition);
                        else
                        {
                            bool found = false;

                            size_t vectorSize = yOffsetVector.size();
                            for (size_t i = 0; i < vectorSize; i++)
                            {
                                if (yOffsetVector[i] - 10 < dotPosition && yOffsetVector[i] + 10 > dotPosition)
                                {
                                    bool withOffsetFound = false;

                                    for (size_t j = 0; j < vectorSize; j++)
                                    {
                                        if (dotPosition + LSPACE - 10 < yOffsetVector[j] && dotPosition + LSPACE + 10 > yOffsetVector[j])
                                            withOffsetFound = true;
                                    }

                                    if (!withOffsetFound)
                                    {
                                        note->getDot()->addOffsetY(LSPACE);
                                        yOffsetVector.push_back(dotPosition + LSPACE);
                                        found = true;

                                        break;
                                    }
                                    else
                                    {
                                        withOffsetFound = false;

                                        for (size_t j = 0; j < vectorSize; j++)
                                        {
                                            if (dotPosition - LSPACE - 10 < yOffsetVector[j] && dotPosition - LSPACE + 10 > yOffsetVector[j])
                                                withOffsetFound = true;
                                        }

                                        if (!withOffsetFound)
                                        {
                                            note->getDot()->addOffsetY(-LSPACE);
                                            yOffsetVector.push_back(dotPosition - LSPACE);
                                            found = true;

                                            break;
                                        }
                                    }
                                }
                            }

                            if (!found)
                                yOffsetVector.push_back(dotPosition);
                        }
                    }
                }

				prevHeadState = retHeadState;
				prevposy = note->getPosition().y;
				if (tagtype == GRTag::SYSTEMTAG)
					prevposy += note->getGRStaff()->getPosition().y;
				
			 	note->updateBoundingBox();
			}
		}


		// - Adjust stem length if it's a cross/triangle notehead

		if (fHigherNote)
		{
			ConstMusicalSymbolID higherNoteSymbol = fHigherNote->getNoteHead()->getSymbol();

			if (higherNoteSymbol == kFullXHeadSymbol)
			{
				fHigherNote->setFirstSegmentDrawingState(false);
				fHigherNote->setStemOffsetStartPosition(4);
			}
			else if (higherNoteSymbol == kFullTriangleHeadSymbol || higherNoteSymbol == kHalfTriangleHeadSymbol)
				fHigherNote->setStemOffsetStartPosition(47);
			else if (higherNoteSymbol == kFullReversedTriangleHeadSymbol || higherNoteSymbol == kHalfReversedTriangleHeadSymbol)
			fHigherNote->setFirstSegmentDrawingState(false);
			// - Set notehead orientation for extreme chord note
			fStem->setLastHeadOrientation (fHigherNote->getHeadState());
		}
	}

		/* To horizontally adjust every dot */
	if (differentOffsets)
	{
		GuidoPos pos = mAssociated->GetHeadPosition();
		while (pos)
		{
			note = dynamic_cast<GRSingleNote *>(mAssociated->GetNext(pos));
			if (note)
			{
				GRNoteDot *currentDot = dynamic_cast<GRNoteDot *>(note->getDot());

				if (currentDot && note->getNoteHead())
				{
					if (note->getNoteHead()->getOffset().x != offsetMax)
						currentDot->addOffsetX(55); //hardcoded
				}
			}
		}
	}
}

void GRGlobalStem::setHPosition( float nx )
{
	if (error)	return; 

	if (tagtype == GRTag::SYSTEMTAG && !mIsSystemCall)
		return;

	// the first tells the element itself of its new position
	// and also the associated elements ... 
	GRPTagARNotationElement::setHPosition(nx + getOffset().x);
	
	if (fStem)	fStem->setHPosition(nx);
	if (fFlag)	fFlag->setHPosition(nx);
}

void GRGlobalStem::OnDraw( VGDevice & hdc) const
{
	if (!mDraw || error) return;

	if (fStem) fStem->OnDraw(hdc);
	if (fFlag) fFlag->OnDraw(hdc);
}

float GRGlobalStem::changeStemLength( float inLen )
{
	if (fStemlengthSet) {
		GuidoWarn("Stemlength already set!");
		if (fStem) return fStem->getStemLength();
	}
	else if (fStem) {
		fStem->setStemLength(inLen);
		return fStem->getStemLength();
	}
	return 0;
}

void GRGlobalStem::tellPosition(GObject * obj, const NVPoint & pt)
{
	if (error)	 return;

	if (dynamic_cast<GRNotationElement *>(obj) == fFirstEl) // useless cast ?
	{
		if (mIsSystemCall) {
			// this is the staff, to which the stem belongs ....
			const GRStaff * stemstaff = fFirstEl->getGRStaff();
			// update the position of the stem and of the flag ....
			
			// determine the lowest and highest position ...
			GRNotationElement * el = mAssociated->GetTail();
			NVPoint offset;
			if (el) {
				offset = el->getGRStaff()->getPosition();
				fLowestY = el->getPosition().y + offset.y;
				fHighestY = fLowestY;
			}
			else {
				fLowestY  = 0;
				fHighestY = 0;
			}
			
			GuidoPos pos = mAssociated->GetHeadPosition();
			while (pos)
			{
				GRNotationElement * el = mAssociated->GetNext(pos);
				if (el && !dynamic_cast<GREmpty *>(el)) {
					offset = el->getGRStaff()->getPosition();
					float ey = el->getPosition().y + offset.y;
					if (fLowestY > ey)	fLowestY = ey;
					if (fHighestY < ey)	fHighestY = ey;
				}
			}

			offset = stemstaff->getPosition();
			fLowestY -= offset.y;
			fHighestY -= offset.y;
	
			GDirection stemdir = fStem->getStemDir();
			// now we have the position of the lowest or highest note ...
			if (stemdir == dirUP)
				fStem->setPosition(NVPoint(0, (GCoord)fHighestY));
			else if (stemdir == dirDOWN)
				fStem->setPosition(NVPoint(0, (GCoord)fLowestY));
			
			// now we have to deal with the length ...
			if (fStemState && fStemState->getLength()->TagIsSet()) {
				// we have a length, that was definitly set ....
				fStem->setStemLength( fStemState->getLength()->getValue());
				fStemlengthSet = true;
			}
			else {
				// length was not set ....
				const float theLength = (float)(fHighestY - fLowestY + stemstaff->getStaffLSPACE() * 3.5f);
				fStem->setStemLength(theLength);
			}
			
			if (stemdir == dirUP)			fFlag->setPosition(NVPoint(0, (GCoord)fHighestY));
			else if (stemdir == dirDOWN)	fFlag->setPosition(NVPoint(0, (GCoord)fLowestY));

			GRNotationElement * tmpel = mAssociated->GetHead();
			if (tmpel)
				updateGlobalStem(tmpel->getGRStaff());
		}
		setHPosition(pt.x);
	}
}

/** \brief Called when the linked GRSystemTag gets a position-update
*/
void GRGlobalStem::checkPosition(const GRSystem * grsys)
{
	if (error) return;

	mIsSystemCall = true;
	tellPosition( fFirstEl, fFirstEl->getPosition());
	mIsSystemCall = false;
}

int GRGlobalStem::getNumFaehnchen() const
{
	if (fFlag)
		return fFlag->getNumFaehnchen();

	// construct a temporary flag to get the num faehnchen that would be present if there were a flag yet.
	// the problem is, that the flag is constructed at the very end of the \shareStem-Range
	// and getNumFaehnchen() is called right after the event was created ....
	GRFlag tmpflag (fDispdur);
	return tmpflag.getNumFaehnchen();
}

void GRGlobalStem::setFlagOnOff(bool i)
{
	fFlagOnOff = i;
	if (fFlag)
		fFlag->setFlagOnOff(i);
}

void GRGlobalStem::setStemDirection(GDirection dir)
{
	const GDirection olddir = fStemdir;
	fStemdir = dir;
	if (fStem)
	{
		fStem->setStemDir(dir);
		if (olddir != fStemdir)
		{
			const float length = fStem->getStemLength();
			float diffy = (float)(fHighestY - fLowestY);
			fStem->setStemLength(length + diffy);
			NVPoint myposition = fStem->getPosition();
			if (fStemdir == dirUP)
				myposition.y += diffy;
			else if (fStemdir == dirDOWN)
				myposition.y -= diffy;

			fStem->setPosition(myposition);
		}
	}
}

NVPoint GRGlobalStem::getStemStartPos() const
{ 
	const GRStem * stem = fStem;
	NVPoint pnt( mPosition );
	GDirection stemdir;
	float stemlength;
	float notebreite = 60;
	if (stem)
	{
		pnt = stem->getPosition();
		stemdir = fStem->getStemDir();
		stemlength = fStem->getStemLength();
		if (stemdir == dirUP)
		{
			pnt.x += (GCoord)(notebreite * 0.5f * mTagSize + mTagOffset.x - (4 * mTagSize));
			pnt.y -= (GCoord)(stemlength - mTagOffset.y);
		}
		else if (stemdir == dirDOWN)
		{		
			pnt.x -= (GCoord)(notebreite * 0.5f * mTagSize) - mTagOffset.x;
			pnt.y += (GCoord)(stemlength +  mTagOffset.y);
		}
	}
	return pnt;

}

NVPoint GRGlobalStem::getStemEndPos() const
{
	const GRStem * stem = fStem;
	GDirection stemdir;
	float notebreite = 60;
	float stemlength;
	NVPoint pnt( mPosition );
	if (stem)
	{
		pnt = stem->getPosition();
		stemdir = fStem->getStemDir();
		stemlength = fStem->getStemLength();
		if (stemdir == dirUP)
		{
			pnt.x += (GCoord)((notebreite * 0.5f * mTagSize) + mTagOffset.x - (1 * mTagSize));
			pnt.y -= (GCoord)(stemlength - mTagOffset.y);
		}
		else if (stemdir == dirDOWN)
		{		
			pnt.x -= (GCoord)((notebreite * 0.5f * mTagSize) - mTagOffset.x - (4 * mTagSize));
			pnt.y += (GCoord)(stemlength + mTagOffset.y);
		}
	}
	return pnt;
}

float GRGlobalStem::getStemLength() const
{
	return fStem ? fStem->getStemLength() : 0;
}


/** \brief (not implemented) This sets the stemlength from the perspective
	 of a note ....

 	what needs to be done: I have to see what direction I am in and
	I then have to set the position of the event in relation to the
	 position of the stem.
*/
void GRGlobalStem::setNoteStemLength( GREvent * ev, float inLen )
{
}

void GRGlobalStem::setSize( float newsize )
{
	mTagSize = newsize;
}

void GRGlobalStem::setMultiplicatedSize(float newMultiplicatedSize)
{
    mTagSize *= newMultiplicatedSize;
}

void GRGlobalStem::setOffsetXY(float inOffsetX, float inOffsetY)
{
    mTagOffset.x += inOffsetX;
    mTagOffset.y += inOffsetY;
}

/** \brief Returns the highest and lowest notehead
	(with respect to y-position.
	this is needed for slurs from and to chords.
*/
int GRGlobalStem::getHighestAndLowestNoteHead(GRStdNoteHead ** highest,
											  GRStdNoteHead ** lowest) const
{
	*highest = *lowest = NULL;
	if (mAssociated == 0 )	return 0;

	GuidoPos pos = mAssociated->GetHeadPosition();
	while (pos)
	{
		GRSingleNote * sn = dynamic_cast<GRSingleNote *>(mAssociated->GetNext(pos));
		if (sn)
		{
			GRStdNoteHead * tmp = sn->getNoteHead();
			if (tmp)
			{
				if (*highest && *lowest)
				{
					if ((*highest)->getPosition().y > tmp->getPosition().y)
					{
						*highest = tmp;
					}
					if ((*lowest)->getPosition().y < tmp->getPosition().y)
					{
						*lowest = tmp;
					}
				}
				else 
				{
					*highest = tmp;
					*lowest = tmp;
				}
			}
		}
	}
	if (*highest != NULL)
		return 1;
	
	return 0;	// is it a stem dir ?
}
