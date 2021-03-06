/*!
 * \copy
 *     Copyright (c)  2009-2013, Cisco Systems
 *     All rights reserved.
 *
 *     Redistribution and use in source and binary forms, with or without
 *     modification, are permitted provided that the following conditions
 *     are met:
 *
 *        * Redistributions of source code must retain the above copyright
 *          notice, this list of conditions and the following disclaimer.
 *
 *        * Redistributions in binary form must reproduce the above copyright
 *          notice, this list of conditions and the following disclaimer in
 *          the documentation and/or other materials provided with the
 *          distribution.
 *
 *     THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *     "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *     LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *     FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *     COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *     INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *     BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *     LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *     CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *     LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *     ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *     POSSIBILITY OF SUCH DAMAGE.
 *
 *
 * \file    svc_encode_slice.c
 *
 * \brief   svc encoding slice
 *
 * \date    2009.07.27 Created
 *
 *************************************************************************************
 */

#include "ls_defines.h"
#include "svc_encode_slice.h"
#include "svc_enc_golomb.h"
#include "svc_base_layer_md.h"
#include "svc_encode_mb.h"
#include "svc_set_mb_syn.h"
#include "decode_mb_aux.h"
#include "svc_mode_decision.h"

namespace WelsEnc {
//#define ENC_TRACE

typedef int32_t (*PWelsCodingSliceFunc) (sWelsEncCtx* pCtx, SSlice* pSlice);
typedef void (*PWelsSliceHeaderWriteFunc) (sWelsEncCtx* pCtx, SBitStringAux* pBs, SDqLayer* pCurLayer, SSlice* pSlice,
    IWelsParametersetStrategy* pParametersetStrategy);

void UpdateNonZeroCountCache (SMB* pMb, SMbCache* pMbCache) {
  ST32 (&pMbCache->iNonZeroCoeffCount[9], LD32 (&pMb->pNonZeroCount[ 0]));
  ST32 (&pMbCache->iNonZeroCoeffCount[17], LD32 (&pMb->pNonZeroCount[ 4]));
  ST32 (&pMbCache->iNonZeroCoeffCount[25], LD32 (&pMb->pNonZeroCount[ 8]));
  ST32 (&pMbCache->iNonZeroCoeffCount[33], LD32 (&pMb->pNonZeroCount[12]));

  ST16 (&pMbCache->iNonZeroCoeffCount[14], LD16 (&pMb->pNonZeroCount[16]));
  ST16 (&pMbCache->iNonZeroCoeffCount[38], LD16 (&pMb->pNonZeroCount[18]));
  ST16 (&pMbCache->iNonZeroCoeffCount[22], LD16 (&pMb->pNonZeroCount[20]));
  ST16 (&pMbCache->iNonZeroCoeffCount[46], LD16 (&pMb->pNonZeroCount[22]));
}

void WelsSliceHeaderScalExtInit (SDqLayer* pCurLayer, SSlice* pSlice) {
  SSliceHeaderExt* pSliceHeadExt = &pSlice->sSliceHeaderExt;
  SNalUnitHeaderExt* pNalHeadExt = &pCurLayer->sLayerInfo.sNalHeaderExt;

  uint8_t uiDependencyId = pNalHeadExt->uiDependencyId;

  pSliceHeadExt->bSliceSkipFlag = false;

  if (uiDependencyId > 0) { //spatial EL
    //bothe adaptive and default flags should equal to 0.
    pSliceHeadExt->bAdaptiveBaseModeFlag     =
      pSliceHeadExt->bAdaptiveMotionPredFlag   =
        pSliceHeadExt->bAdaptiveResidualPredFlag = false;

    pSliceHeadExt->bDefaultBaseModeFlag     =
      pSliceHeadExt->bDefaultMotionPredFlag   =
        pSliceHeadExt->bDefaultResidualPredFlag = false;
  }
}

void WelsSliceHeaderExtInit (sWelsEncCtx* pEncCtx, SDqLayer* pCurLayer, SSlice* pSlice) {
  SSliceHeaderExt* pCurSliceExt = &pSlice->sSliceHeaderExt;
  SSliceHeader* pCurSliceHeader  = &pCurSliceExt->sSliceHeader;
  SSpatialLayerInternal* pParamInternal = &pEncCtx->pSvcParam->sDependencyLayers[pEncCtx->uiDependencyId];
  pCurSliceHeader->eSliceType = pEncCtx->eSliceType;

  pCurSliceExt->bStoreRefBasePicFlag = false;

  pCurSliceHeader->iFirstMbInSlice = WelsGetFirstMbOfSlice (pCurLayer->ppSliceInLayer, pSlice->uiSliceIdx);

  pCurSliceHeader->iFrameNum      = pParamInternal->iFrameNum;
  pCurSliceHeader->uiIdrPicId     = pParamInternal->uiIdrPicId;
  pCurSliceHeader->iPicOrderCntLsb = pEncCtx->pEncPic->iFramePoc;      // 0

  if (P_SLICE == pEncCtx->eSliceType) {
    pCurSliceHeader->uiNumRefIdxL0Active = 1;
    if (pCurSliceHeader->uiRefCount > 0 &&
        pCurSliceHeader->uiRefCount < pCurLayer->sLayerInfo.pSpsP->iNumRefFrames) {
      pCurSliceHeader->bNumRefIdxActiveOverrideFlag = true;
      pCurSliceHeader->uiNumRefIdxL0Active = pCurSliceHeader->uiRefCount;
    }
    //to solve mismatch between debug&release
    else {
      pCurSliceHeader->bNumRefIdxActiveOverrideFlag = false;
    }
  }

  pCurSliceHeader->iSliceQpDelta = pEncCtx->iGlobalQp - pCurLayer->sLayerInfo.pPpsP->iPicInitQp;

  //for deblocking initial
  pCurSliceHeader->uiDisableDeblockingFilterIdc = pCurLayer->iLoopFilterDisableIdc;
  pCurSliceHeader->iSliceAlphaC0Offset =
    pCurLayer->iLoopFilterAlphaC0Offset; // need update iSliceAlphaC0Offset & iSliceBetaOffset for pSlice-header if loop_filter_idc != 1
  pCurSliceHeader->iSliceBetaOffset = pCurLayer->iLoopFilterBetaOffset;
  pCurSliceExt->uiDisableInterLayerDeblockingFilterIdc = pCurLayer->uiDisableInterLayerDeblockingFilterIdc;

  if (pSlice->bSliceHeaderExtFlag) {
    WelsSliceHeaderScalExtInit (pCurLayer, pSlice);
  } else {
    //both adaptive and default flags should equal to 0.
    pCurSliceExt->bAdaptiveBaseModeFlag =
      pCurSliceExt->bAdaptiveMotionPredFlag =
        pCurSliceExt->bAdaptiveResidualPredFlag = false;

    pCurSliceExt->bDefaultBaseModeFlag =
      pCurSliceExt->bDefaultMotionPredFlag =
        pCurSliceExt->bDefaultResidualPredFlag  = false;
  }
}


void UpdateMbNeighbor (SDqLayer* pCurDq, SMB* pMb, const int32_t kiMbWidth, uint16_t uiSliceIdc) {
  uint32_t uiNeighborAvailFlag        = 0;
  const int32_t kiMbXY                = pMb->iMbXY;
  const int32_t kiMbX                 = pMb->iMbX;
  const int32_t kiMbY                 = pMb->iMbY;
  bool     bLeft;
  bool     bTop;
  bool     bLeftTop;
  bool     bRightTop;
  int32_t   iLeftXY, iTopXY, iLeftTopXY, iRightTopXY;

  pMb->uiSliceIdc = uiSliceIdc;
  iLeftXY = kiMbXY - 1;
  iTopXY = kiMbXY - kiMbWidth;
  iLeftTopXY = iTopXY - 1;
  iRightTopXY = iTopXY + 1;

  bLeft = (kiMbX > 0) && (uiSliceIdc == WelsMbToSliceIdc (pCurDq, iLeftXY));
  bTop = (kiMbY > 0) && (uiSliceIdc == WelsMbToSliceIdc (pCurDq, iTopXY));
  bLeftTop = (kiMbX > 0) && (kiMbY > 0) && (uiSliceIdc == WelsMbToSliceIdc (pCurDq, iLeftTopXY));
  bRightTop = (kiMbX < (kiMbWidth - 1)) && (kiMbY > 0) && (uiSliceIdc == WelsMbToSliceIdc (pCurDq, iRightTopXY));

  if (bLeft) {
    uiNeighborAvailFlag |= LEFT_MB_POS;
  }
  if (bTop) {
    uiNeighborAvailFlag |= TOP_MB_POS;
  }
  if (bLeftTop) {
    uiNeighborAvailFlag |= TOPLEFT_MB_POS;
  }
  if (bRightTop) {
    uiNeighborAvailFlag |= TOPRIGHT_MB_POS;
  }
  pMb->uiNeighborAvail = (uint8_t)uiNeighborAvailFlag;
}

/* count MB types if enabled FRAME_INFO_OUTPUT*/
#if defined(MB_TYPES_CHECK)
void WelsCountMbType (int32_t (*iMbCount)[18], const EWelsSliceType keSt, const SMB* kpMb) {
  if (NULL == iMbCount)
    return;

  switch (kpMb->uiMbType) {
  case MB_TYPE_INTRA4x4:
    ++ iMbCount[keSt][Intra4x4];
    break;
  case MB_TYPE_INTRA16x16:
    ++ iMbCount[keSt][Intra16x16];
    break;
  case MB_TYPE_SKIP:
    ++ iMbCount[keSt][PSkip];
    break;
  case MB_TYPE_16x16:
    ++ iMbCount[keSt][Inter16x16];
    break;
  case MB_TYPE_16x8:
    ++ iMbCount[keSt][Inter16x8];
    break;
  case MB_TYPE_8x16:
    ++ iMbCount[keSt][Inter8x16];
    break;
  case MB_TYPE_8x8:
    ++ iMbCount[keSt][Inter8x8];
    break;
  case MB_TYPE_INTRA_BL:
    ++ iMbCount[keSt][7];
    break;
  default:
    break;
  }
}
#endif//MB_TYPES_CHECK

/*!
* \brief    write reference picture list on reordering syntax in Slice header
*/
void WriteReferenceReorder (SBitStringAux* pBs, SSliceHeader* sSliceHeader) {
  SRefPicListReorderSyntax* pRefOrdering    = &sSliceHeader->sRefReordering;
  uint8_t eSliceType                        = sSliceHeader->eSliceType % 5;
  int16_t n = 0;

  if (I_SLICE != eSliceType && SI_SLICE != eSliceType) { // !I && !SI
    BsWriteOneBit (pBs, true);
//    {
    uint16_t uiReorderingOfPicNumsIdc;
    do {
      uiReorderingOfPicNumsIdc = pRefOrdering->SReorderingSyntax[n].uiReorderingOfPicNumsIdc;
      BsWriteUE (pBs, uiReorderingOfPicNumsIdc);
      if (0 == uiReorderingOfPicNumsIdc || 1 == uiReorderingOfPicNumsIdc)
        BsWriteUE (pBs, pRefOrdering->SReorderingSyntax[n].uiAbsDiffPicNumMinus1);
      else if (2 == uiReorderingOfPicNumsIdc)
        BsWriteUE (pBs, pRefOrdering->SReorderingSyntax[n].iLongTermPicNum);

      n ++;
    } while (3 != uiReorderingOfPicNumsIdc);
//    }
  }
}

/*!
* \brief    write reference picture marking syntax in pSlice header
*/
void WriteRefPicMarking (SBitStringAux* pBs, SSliceHeader* pSliceHeader, SNalUnitHeaderExt* pNalHdrExt) {
  SRefPicMarking* sRefMarking = &pSliceHeader->sRefMarking;
  int16_t n = 0;

  if (pNalHdrExt->bIdrFlag) {
    BsWriteOneBit (pBs, sRefMarking->bNoOutputOfPriorPicsFlag);
    BsWriteOneBit (pBs, sRefMarking->bLongTermRefFlag);
  } else {
    BsWriteOneBit (pBs, sRefMarking->bAdaptiveRefPicMarkingModeFlag);

    if (sRefMarking->bAdaptiveRefPicMarkingModeFlag) {
      int32_t iMmcoType;
      do {
        iMmcoType = sRefMarking->SMmcoRef[n].iMmcoType;
        BsWriteUE (pBs, iMmcoType);
        if (1 == iMmcoType || 3 == iMmcoType)
          BsWriteUE (pBs, sRefMarking->SMmcoRef[n].iDiffOfPicNum - 1);

        if (2 == iMmcoType)
          BsWriteUE (pBs, sRefMarking->SMmcoRef[n].iLongTermPicNum);

        if (3 == iMmcoType || 6 == iMmcoType)
          BsWriteUE (pBs, sRefMarking->SMmcoRef[n].iLongTermFrameIdx);

        if (4 == iMmcoType)
          BsWriteUE (pBs, sRefMarking->SMmcoRef[n].iMaxLongTermFrameIdx + 1);

        n ++;
      } while (0 != iMmcoType);
    }

  }
}

void WelsSliceHeaderWrite (sWelsEncCtx* pCtx, SBitStringAux* pBs, SDqLayer* pCurLayer, SSlice* pSlice,
                           IWelsParametersetStrategy* pParametersetStrategy) {
  SWelsSPS* pSps = pCurLayer->sLayerInfo.pSpsP;
  SWelsPPS* pPps = pCurLayer->sLayerInfo.pPpsP;
  SSliceHeader* pSliceHeader      = &pSlice->sSliceHeaderExt.sSliceHeader;
  SNalUnitHeaderExt* pNalHead   = &pCurLayer->sLayerInfo.sNalHeaderExt;

  BsWriteUE (pBs, pSliceHeader->iFirstMbInSlice);
  BsWriteUE (pBs, pSliceHeader->eSliceType);    /* same type things */

  BsWriteUE (pBs, pSliceHeader->pPps->iPpsId + pParametersetStrategy->GetPpsIdOffset (pSliceHeader->pPps->iPpsId));

  BsWriteBits (pBs, pSps->uiLog2MaxFrameNum, pSliceHeader->iFrameNum);

  if (pNalHead->bIdrFlag) { /* NAL IDR */
    BsWriteUE (pBs, pSliceHeader->uiIdrPicId);
  }

  BsWriteBits (pBs, pSps->iLog2MaxPocLsb, pSliceHeader->iPicOrderCntLsb);

  if (P_SLICE == pSliceHeader->eSliceType) {
    BsWriteOneBit (pBs, pSliceHeader->bNumRefIdxActiveOverrideFlag);
    if (pSliceHeader->bNumRefIdxActiveOverrideFlag) {
      BsWriteUE (pBs, WELS_CLIP3 (pSliceHeader->uiNumRefIdxL0Active - 1, 0, MAX_REF_PIC_COUNT));
    }
  }

  if (!pNalHead->bIdrFlag)
    WriteReferenceReorder (pBs, pSliceHeader);

  if (pNalHead->sNalUnitHeader.uiNalRefIdc) {
    WriteRefPicMarking (pBs, pSliceHeader, pNalHead);
  }

  if (pPps->bEntropyCodingModeFlag && pSliceHeader->eSliceType  != I_SLICE) {
    BsWriteUE (pBs, pSlice->iCabacInitIdc);
  }
  BsWriteSE (pBs, pSliceHeader->iSliceQpDelta);       /* pSlice qp delta */

  if (pPps->bDeblockingFilterControlPresentFlag) {
    switch (pSliceHeader->uiDisableDeblockingFilterIdc) {
    case 0:
    case 3:
    case 4:
    case 6:
      BsWriteUE (pBs, 0);
      break;
    case 1:
      BsWriteUE (pBs, 1);
      break;
    case 2:
    case 5:
      BsWriteUE (pBs, 2);
      break;
    default:
      WelsLog (&pCtx->sLogCtx, WELS_LOG_ERROR, "Invalid uiDisableDeblockingFilterIdc %d",
               pSliceHeader->uiDisableDeblockingFilterIdc);
      break;
    }
    if (1 != pSliceHeader->uiDisableDeblockingFilterIdc) {
      BsWriteSE (pBs, pSliceHeader->iSliceAlphaC0Offset >> 1);
      BsWriteSE (pBs, pSliceHeader->iSliceBetaOffset >> 1);
    }
  }
}

void WelsSliceHeaderExtWrite (sWelsEncCtx* pCtx, SBitStringAux* pBs, SDqLayer* pCurLayer, SSlice* pSlice,
                              IWelsParametersetStrategy* pParametersetStrategy) {
  SWelsSPS* pSps           = pCurLayer->sLayerInfo.pSpsP;
  SWelsPPS* pPps           = pCurLayer->sLayerInfo.pPpsP;
  SSubsetSps* pSubSps = pCurLayer->sLayerInfo.pSubsetSpsP;
  SSliceHeaderExt* pSliceHeadExt = &pSlice->sSliceHeaderExt;
  SSliceHeader* pSliceHeader      = &pSliceHeadExt->sSliceHeader;
  SNalUnitHeaderExt* pNalHead   = &pCurLayer->sLayerInfo.sNalHeaderExt;

  BsWriteUE (pBs, pSliceHeader->iFirstMbInSlice);
  BsWriteUE (pBs, pSliceHeader->eSliceType);    /* same type things */

  BsWriteUE (pBs, pSliceHeader->pPps->iPpsId +
             pParametersetStrategy->GetPpsIdOffset (pSliceHeader->pPps->iPpsId));

  BsWriteBits (pBs, pSps->uiLog2MaxFrameNum, pSliceHeader->iFrameNum);

  if (pNalHead->bIdrFlag) { /* NAL IDR */
    BsWriteUE (pBs, pSliceHeader->uiIdrPicId);
  }

  BsWriteBits (pBs, pSps->iLog2MaxPocLsb, pSliceHeader->iPicOrderCntLsb);
//  {
  if (P_SLICE == pSliceHeader->eSliceType) {
    BsWriteOneBit (pBs, pSliceHeader->bNumRefIdxActiveOverrideFlag);
    if (pSliceHeader->bNumRefIdxActiveOverrideFlag) {
      BsWriteUE (pBs, WELS_CLIP3 (pSliceHeader->uiNumRefIdxL0Active - 1, 0, MAX_REF_PIC_COUNT));
    }
  }

  if (!pNalHead->bIdrFlag)
    WriteReferenceReorder (pBs, pSliceHeader);

  if (pNalHead->sNalUnitHeader.uiNalRefIdc) {
    WriteRefPicMarking (pBs, pSliceHeader, pNalHead);

    if (!pSubSps->sSpsSvcExt.bSliceHeaderRestrictionFlag) {
      BsWriteOneBit (pBs, pSliceHeadExt->bStoreRefBasePicFlag);
    }
  }
//  }


  if (pPps->bEntropyCodingModeFlag && pSliceHeader->eSliceType  != I_SLICE) {
    BsWriteUE (pBs, pSlice->iCabacInitIdc);
  }

  BsWriteSE (pBs, pSliceHeader->iSliceQpDelta);       /* pSlice qp delta */

  if (pPps->bDeblockingFilterControlPresentFlag) {
    BsWriteUE (pBs, pSliceHeader->uiDisableDeblockingFilterIdc);
    if (1 != pSliceHeader->uiDisableDeblockingFilterIdc) {
      BsWriteSE (pBs, pSliceHeader->iSliceAlphaC0Offset >> 1);
      BsWriteSE (pBs, pSliceHeader->iSliceBetaOffset >> 1);
    }
  }

#if !defined(DISABLE_FMO_FEATURE)
  if (pPps->uiNumSliceGroups > 1  &&
      pPps->uiSliceGroupMapType >= 3 &&
      pPps->uiSliceGroupMapType <= 5) {
    int32_t iNumBits;
    if (pPps->uiSliceGroupChangeRate) {
      iNumBits = WELS_CEILLOG2 (1 + pPps->uiPicSizeInMapUnits / pPps->uiSliceGroupChangeRate);
      BsWriteBits (pBs, iNumBits, pSliceHeader->iSliceGroupChangeCycle);
    }
  }
#endif//!DISABLE_FMO_FEATURE

  if (false) {
    BsWriteOneBit (pBs, pSliceHeadExt->bSliceSkipFlag);
    if (pSliceHeadExt->bSliceSkipFlag) {
      BsWriteUE (pBs, pSliceHeadExt->uiNumMbsInSlice - 1);
    } else {
      BsWriteOneBit (pBs, pSliceHeadExt->bAdaptiveBaseModeFlag);
      if (!pSliceHeadExt->bAdaptiveBaseModeFlag) {
        BsWriteOneBit (pBs, pSliceHeadExt->bDefaultBaseModeFlag);
      }

      if (!pSliceHeadExt->bDefaultBaseModeFlag) {
        BsWriteOneBit (pBs, 0);
        BsWriteOneBit (pBs, 0);
      }

      BsWriteOneBit (pBs, pSliceHeadExt->bAdaptiveResidualPredFlag);
      if (!pSliceHeadExt->bAdaptiveResidualPredFlag) {
        BsWriteOneBit (pBs, 0);
      }
    }
    if (1 == pSubSps->sSpsSvcExt.bAdaptiveTcoeffLevelPredFlag) {
      BsWriteOneBit (pBs, pSliceHeadExt->bTcoeffLevelPredFlag);
    }

  }

  if (!pSubSps->sSpsSvcExt.bSliceHeaderRestrictionFlag) {
    BsWriteBits (pBs, 4, 0);
    BsWriteBits (pBs, 4, 15);
  }
}

//only BaseLayer inter MB and SpatialLayer (uiQualityId = 0) inter MB calling this pFunc.
//only for inter part
void WelsInterMbEncode (sWelsEncCtx* pEncCtx, SSlice* pSlice, SMB* pCurMb) {
  SMbCache* pMbCache = &pSlice->sMbCacheInfo;

  WelsDctMb (pMbCache->pCoeffLevel,  pMbCache->SPicData.pEncMb[0], pEncCtx->pCurDqLayer->iEncStride[0],
             pMbCache->pMemPredLuma, pEncCtx->pFuncList->pfDctFourT4);
  WelsEncInterY (pEncCtx->pFuncList, pCurMb, pMbCache);
}


//only BaseLayer inter MB and SpatialLayer (uiQualityId = 0) inter MB calling this pFunc.
//only for I SSlice
void WelsIMbChromaEncode (sWelsEncCtx* pEncCtx, SMB* pCurMb, SMbCache* pMbCache) {
  SWelsFuncPtrList* pFunc       = pEncCtx->pFuncList;
  SDqLayer* pCurLayer           = pEncCtx->pCurDqLayer;
  const int32_t kiEncStride     = pCurLayer->iEncStride[1];
  const int32_t kiCsStride      = pCurLayer->iCsStride[1];
  int16_t* pCurRS               = pMbCache->pCoeffLevel;
  uint8_t* pBestPred            = pMbCache->pBestPredIntraChroma;
  uint8_t* pCsCb                = pMbCache->SPicData.pCsMb[1];
  uint8_t* pCsCr                = pMbCache->SPicData.pCsMb[2];

  //cb
  pFunc->pfDctFourT4 (pCurRS,    pMbCache->SPicData.pEncMb[1], kiEncStride, pBestPred,    8);
  WelsEncRecUV (pFunc, pCurMb, pMbCache, pCurRS,    1);
  pFunc->pfIDctFourT4 (pCsCb, kiCsStride, pBestPred,    8, pCurRS);

  //cr
  pFunc->pfDctFourT4 (pCurRS + 64, pMbCache->SPicData.pEncMb[2], kiEncStride, pBestPred + 64, 8);
  WelsEncRecUV (pFunc, pCurMb, pMbCache, pCurRS + 64, 2);
  pFunc->pfIDctFourT4 (pCsCr, kiCsStride, pBestPred + 64, 8, pCurRS + 64);
}


//only BaseLayer inter MB and SpatialLayer (uiQualityId = 0) inter MB calling this pFunc.
//for P SSlice (intra part + inter part)
void WelsPMbChromaEncode (sWelsEncCtx* pEncCtx, SSlice* pSlice, SMB* pCurMb) {
  SWelsFuncPtrList* pFunc       = pEncCtx->pFuncList;
  SDqLayer* pCurLayer           = pEncCtx->pCurDqLayer;
  const int32_t kiEncStride     = pCurLayer->iEncStride[1];
  SMbCache* pMbCache            = &pSlice->sMbCacheInfo;
  int16_t* pCurRS               = pMbCache->pCoeffLevel + 256;
  uint8_t* pBestPred            = pMbCache->pMemPredChroma;

  pFunc->pfDctFourT4 (pCurRS,       pMbCache->SPicData.pEncMb[1],   kiEncStride,    pBestPred,      8);
  pFunc->pfDctFourT4 (pCurRS + 64,  pMbCache->SPicData.pEncMb[2],   kiEncStride,    pBestPred + 64, 8);

  WelsEncRecUV (pFunc, pCurMb, pMbCache, pCurRS, 1);
  WelsEncRecUV (pFunc, pCurMb, pMbCache, pCurRS + 64, 2);
}

void OutputPMbWithoutConstructCsRsNoCopy (sWelsEncCtx* pCtx, SDqLayer* pDq, SSlice* pSlice, SMB* pMb) {
  if ((IS_INTER (pMb->uiMbType) && !IS_SKIP (pMb->uiMbType))
      || IS_I_BL (pMb->uiMbType)) { //intra have been reconstructed, NO COPY from CS to pDecPic--
    SMbCache* pMbCache                  = &pSlice->sMbCacheInfo;
    uint8_t* pDecY                      = pMbCache->SPicData.pDecMb[0];
    uint8_t* pDecU                      = pMbCache->SPicData.pDecMb[1];
    uint8_t* pDecV                      = pMbCache->SPicData.pDecMb[2];
    int16_t* pScaledTcoeff              = pMbCache->pCoeffLevel;
    const int32_t kiDecStrideLuma       = pDq->pDecPic->iLineSize[0];
    const int32_t kiDecStrideChroma     = pDq->pDecPic->iLineSize[1];
    PIDctFunc pfIdctFour4x4             = pCtx->pFuncList->pfIDctFourT4;

    WelsIDctT4RecOnMb (pDecY, kiDecStrideLuma, pDecY, kiDecStrideLuma, pScaledTcoeff,  pfIdctFour4x4);
    pfIdctFour4x4 (pDecU, kiDecStrideChroma, pDecU, kiDecStrideChroma, pScaledTcoeff + 256);
    pfIdctFour4x4 (pDecV, kiDecStrideChroma, pDecV, kiDecStrideChroma, pScaledTcoeff + 320);
  }
}

void UpdateQpForOverflow (SMB* pCurMb, uint8_t kuiChromaQpIndexOffset) {
  pCurMb->uiLumaQp += DELTA_QP;
  pCurMb->uiChromaQp = g_kuiChromaQpTable[CLIP3_QP_0_51 (pCurMb->uiLumaQp + kuiChromaQpIndexOffset)];
}
// for intra non-dynamic pSlice
//encapsulate two kinds of reconstruction:
//first. store base or highest Dependency Layer with only one quality (without CS RS reconstruction)
//second. lower than highest Dependency Layer, and for every Dependency Layer with one quality layer(single layer)
int32_t WelsISliceMdEnc (sWelsEncCtx* pEncCtx, SSlice* pSlice) { //pMd + encoding
  SDqLayer* pCurLayer           = pEncCtx->pCurDqLayer;
  SMbCache* pMbCache            = &pSlice->sMbCacheInfo;
  SSliceHeaderExt* pSliceHdExt  = &pSlice->sSliceHeaderExt;
  SMB* pMbList                  = pCurLayer->sMbDataP;
  SMB* pCurMb                   = NULL;
  const int32_t kiSliceFirstMbXY = pSliceHdExt->sSliceHeader.iFirstMbInSlice;
  int32_t iNextMbIdx            = kiSliceFirstMbXY;
  const int32_t kiTotalNumMb    = pCurLayer->iMbWidth * pCurLayer->iMbHeight;
  int32_t iCurMbIdx             = 0, iNumMbCoded = 0;
  const int32_t kiSliceIdx      = pSlice->uiSliceIdx;
  const uint8_t kuiChromaQpIndexOffset = pCurLayer->sLayerInfo.pPpsP->uiChromaQpIndexOffset;

  SWelsMD sMd;
  int32_t iEncReturn = ENC_RETURN_SUCCESS;
  SDynamicSlicingStack sDss;
  if (pEncCtx->pSvcParam->iEntropyCodingModeFlag) {
    WelsInitSliceCabac (pEncCtx, pSlice);
    sDss.pRestoreBuffer = NULL;
    sDss.iStartPos = sDss.iCurrentPos = 0;
  }
  for (; ;) {
    if (!pEncCtx->pSvcParam->iEntropyCodingModeFlag)
      pEncCtx->pFuncList->pfStashMBStatus (&sDss, pSlice, 0);
    iCurMbIdx = iNextMbIdx;
    pCurMb = &pMbList[ iCurMbIdx ];

    pEncCtx->pFuncList->pfRc.pfWelsRcMbInit (pEncCtx, pCurMb, pSlice);
    WelsMdIntraInit (pEncCtx, pCurMb, pMbCache, kiSliceFirstMbXY);

TRY_REENCODING:
    sMd.iLambda = g_kiQpCostTable[pCurMb->uiLumaQp];
    WelsMdIntraMb (pEncCtx, &sMd, pCurMb, pMbCache);
    UpdateNonZeroCountCache (pCurMb, pMbCache);


    iEncReturn = pEncCtx->pFuncList->pfWelsSpatialWriteMbSyn (pEncCtx, pSlice, pCurMb);
    if (!pEncCtx->pSvcParam->iEntropyCodingModeFlag) {
      if ((iEncReturn == ENC_RETURN_VLCOVERFLOWFOUND) && (pCurMb->uiLumaQp < 50)) {
        pEncCtx->pFuncList->pfStashPopMBStatus (&sDss, pSlice);
        UpdateQpForOverflow (pCurMb, kuiChromaQpIndexOffset);
        goto TRY_REENCODING;
      }
    }
    if (ENC_RETURN_SUCCESS != iEncReturn)
      return iEncReturn;

    pCurMb->uiSliceIdc = kiSliceIdx;

#if defined(MB_TYPES_CHECK)
    WelsCountMbType (pEncCtx->sPerInfo.iMbCount, I_SLICE, pCurMb);
#endif//MB_TYPES_CHECK

    pEncCtx->pFuncList->pfRc.pfWelsRcMbInfoUpdate (pEncCtx, pCurMb, sMd.iCostLuma, pSlice);

    ++iNumMbCoded;
    iNextMbIdx = WelsGetNextMbOfSlice (pCurLayer, iCurMbIdx);
    if (iNextMbIdx == -1 || iNextMbIdx >= kiTotalNumMb || iNumMbCoded >= kiTotalNumMb) {
      break;
    }
  }

  return ENC_RETURN_SUCCESS;
}

// Only for intra dynamic slicing
int32_t WelsISliceMdEncDynamic (sWelsEncCtx* pEncCtx, SSlice* pSlice) { //pMd + encoding
  SBitStringAux* pBs            = pSlice->pSliceBsa;
  SDqLayer* pCurLayer           = pEncCtx->pCurDqLayer;
  SSliceCtx* pSliceCtx          = &pCurLayer->sSliceEncCtx;
  SMbCache* pMbCache            = &pSlice->sMbCacheInfo;
  SSliceHeaderExt* pSliceHdExt  = &pSlice->sSliceHeaderExt;
  SMB* pMbList                  = pCurLayer->sMbDataP;
  SMB* pCurMb                   = NULL;
  const int32_t kiSliceFirstMbXY = pSliceHdExt->sSliceHeader.iFirstMbInSlice;
  int32_t iNextMbIdx            = kiSliceFirstMbXY;
  const int32_t kiTotalNumMb    = pCurLayer->iMbWidth * pCurLayer->iMbHeight;
  int32_t iCurMbIdx             = 0, iNumMbCoded = 0;
  const int32_t kiSliceIdx      = pSlice->uiSliceIdx;
  const int32_t kiPartitionId   = (kiSliceIdx % pEncCtx->iActiveThreadsNum);
  const uint8_t kuiChromaQpIndexOffset = pCurLayer->sLayerInfo.pPpsP->uiChromaQpIndexOffset;
  int32_t iEncReturn = ENC_RETURN_SUCCESS;

  SWelsMD sMd;
  SDynamicSlicingStack sDss;
  if (pEncCtx->pSvcParam->iEntropyCodingModeFlag) {
    WelsInitSliceCabac (pEncCtx, pSlice);
    sDss.pRestoreBuffer = pEncCtx->pDynamicBsBuffer[kiPartitionId];
    sDss.iStartPos = sDss.iCurrentPos = 0;
  } else {
    sDss.iStartPos = BsGetBitsPos (pBs);
  }
  for (; ;) {
    iCurMbIdx = iNextMbIdx;
    pCurMb = &pMbList[ iCurMbIdx ];

    pEncCtx->pFuncList->pfStashMBStatus (&sDss, pSlice, 0);
    pEncCtx->pFuncList->pfRc.pfWelsRcMbInit (pEncCtx, pCurMb, pSlice);
    // if already reaches the largest number of slices, set QPs to the upper bound
    if (pSlice->bDynamicSlicingSliceSizeCtrlFlag) {
      pCurMb->uiLumaQp = pEncCtx->pWelsSvcRc[pEncCtx->uiDependencyId].iMaxQp;
      pCurMb->uiChromaQp = g_kuiChromaQpTable[CLIP3_QP_0_51 (pCurMb->uiLumaQp + kuiChromaQpIndexOffset)];
    }
    WelsMdIntraInit (pEncCtx, pCurMb, pMbCache, kiSliceFirstMbXY);

TRY_REENCODING:
    sMd.iLambda = g_kiQpCostTable[pCurMb->uiLumaQp];
    WelsMdIntraMb (pEncCtx, &sMd, pCurMb, pMbCache);
    UpdateNonZeroCountCache (pCurMb, pMbCache);

    iEncReturn = pEncCtx->pFuncList->pfWelsSpatialWriteMbSyn (pEncCtx, pSlice, pCurMb);
    if (iEncReturn == ENC_RETURN_VLCOVERFLOWFOUND && (pCurMb->uiLumaQp < 50)) {
      pEncCtx->pFuncList->pfStashPopMBStatus (&sDss, pSlice);
      UpdateQpForOverflow (pCurMb, kuiChromaQpIndexOffset);
      goto TRY_REENCODING;
    }
    if (ENC_RETURN_SUCCESS != iEncReturn)
      return iEncReturn;

    sDss.iCurrentPos = pEncCtx->pFuncList->pfGetBsPosition (pSlice);

    if (DynSlcJudgeSliceBoundaryStepBack (pEncCtx, pSlice, pSliceCtx, pCurMb, &sDss)) { //islice
      pEncCtx->pFuncList->pfStashPopMBStatus (&sDss, pSlice);
      pCurLayer->pLastCodedMbIdxOfPartition[kiPartitionId] = iCurMbIdx -
          1; // update pLastCodedMbIdxOfPartition, need to -1 due to stepping back
      ++ pCurLayer->pNumSliceCodedOfPartition[kiPartitionId];

      break;
    }


    pCurMb->uiSliceIdc = kiSliceIdx;

#if defined(MB_TYPES_CHECK)
    WelsCountMbType (pEncCtx->sPerInfo.iMbCount, I_SLICE, pCurMb);
#endif//MB_TYPES_CHECK

    pEncCtx->pFuncList->pfRc.pfWelsRcMbInfoUpdate (pEncCtx, pCurMb, sMd.iCostLuma, pSlice);

    ++iNumMbCoded;

    iNextMbIdx = WelsGetNextMbOfSlice (pCurLayer, iCurMbIdx);
    //whether all of MB in current pSlice encoded or not
    if (iNextMbIdx == -1 || iNextMbIdx >= kiTotalNumMb || iNumMbCoded >= kiTotalNumMb) {
      pSlice->iCountMbNumInSlice = iCurMbIdx - pCurLayer->pLastCodedMbIdxOfPartition[kiPartitionId];
      pCurLayer->pLastCodedMbIdxOfPartition[kiPartitionId] =
        iCurMbIdx; // update pLastCodedMbIdxOfPartition, finish coding, use iCurMbIdx directly
      break;
    }
  }
  return iEncReturn;
}

//encapsulate two kinds of reconstruction:
// first. store base or highest Dependency Layer with only one quality (without CS RS reconstruction)
// second. lower than highest Dependency Layer, and for every Dependency Layer with one quality layer(single layer)
int32_t WelsPSliceMdEnc (sWelsEncCtx* pEncCtx, SSlice* pSlice,  const bool kbIsHighestDlayerFlag) { //pMd + encoding
  const SSliceHeaderExt*    kpShExt             = &pSlice->sSliceHeaderExt;
  const SSliceHeader*       kpSh                = &kpShExt->sSliceHeader;
  const int32_t             kiSliceFirstMbXY    = kpSh->iFirstMbInSlice;
  SWelsMD sMd;

  sMd.uiRef         = kpSh->uiRefIndex;
  sMd.bMdUsingSad   = kbIsHighestDlayerFlag;
  if (!pEncCtx->pCurDqLayer->bBaseLayerAvailableFlag || !kbIsHighestDlayerFlag)
    memset (&sMd.sMe, 0, sizeof (sMd.sMe));

  //pMb loop
  return WelsMdInterMbLoop (pEncCtx, pSlice, &sMd, kiSliceFirstMbXY);
}

int32_t WelsPSliceMdEncDynamic (sWelsEncCtx* pEncCtx, SSlice* pSlice, const bool kbIsHighestDlayerFlag) {
  const SSliceHeaderExt*    kpShExt             = &pSlice->sSliceHeaderExt;
  const SSliceHeader*       kpSh                = &kpShExt->sSliceHeader;
  const int32_t             kiSliceFirstMbXY    = kpSh->iFirstMbInSlice;
  SWelsMD sMd;

  sMd.uiRef         = kpSh->uiRefIndex;
  sMd.bMdUsingSad   = kbIsHighestDlayerFlag;
  if (!pEncCtx->pCurDqLayer->bBaseLayerAvailableFlag || !kbIsHighestDlayerFlag)
    memset (&sMd.sMe, 0, sizeof (sMd.sMe));

  //mb loop
  return WelsMdInterMbLoopOverDynamicSlice (pEncCtx, pSlice, &sMd, kiSliceFirstMbXY);
}

int32_t WelsCodePSlice (sWelsEncCtx* pEncCtx, SSlice* pSlice) {
  //pSlice-level init should be outside and before this function
  SDqLayer* pCurLayer = pEncCtx->pCurDqLayer;

  const bool kbBaseAvail = pCurLayer->bBaseLayerAvailableFlag;
  const bool kbHighestSpatial = pEncCtx->pSvcParam->iSpatialLayerNum ==
                                (pCurLayer->sLayerInfo.sNalHeaderExt.uiDependencyId + 1);

  //MD switch
  if (kbBaseAvail && kbHighestSpatial) {
    //initial pMd pointer
    pEncCtx->pFuncList->pfInterMd = WelsMdInterMbEnhancelayer;
  } else {
    //initial pMd pointer
    pEncCtx->pFuncList->pfInterMd = WelsMdInterMb;
  }
  return WelsPSliceMdEnc (pEncCtx, pSlice, kbHighestSpatial);
}

int32_t WelsCodePOverDynamicSlice (sWelsEncCtx* pEncCtx, SSlice* pSlice) {
  //pSlice-level init should be outside and before this function
  SDqLayer* pCurLayer = pEncCtx->pCurDqLayer;

  const bool kbBaseAvail = pCurLayer->bBaseLayerAvailableFlag;
  const bool kbHighestSpatial = pEncCtx->pSvcParam->iSpatialLayerNum ==
                                (pCurLayer->sLayerInfo.sNalHeaderExt.uiDependencyId + 1);

  //MD switch
  if (kbBaseAvail && kbHighestSpatial) {
    //initial pMd pointer
    pEncCtx->pFuncList->pfInterMd = WelsMdInterMbEnhancelayer;
  } else {
    //initial pMd pointer
    pEncCtx->pFuncList->pfInterMd = WelsMdInterMb;
  }
  return WelsPSliceMdEncDynamic (pEncCtx, pSlice, kbHighestSpatial);
}

// 1st index: 0: for P pSlice; 1: for I pSlice;
// 2nd index: 0: for non-dynamic pSlice; 1: for dynamic I pSlice;
static const PWelsCodingSliceFunc g_pWelsSliceCoding[2][2] = {
  { WelsCodePSlice, WelsCodePOverDynamicSlice }, // P SSlice
  { WelsISliceMdEnc, WelsISliceMdEncDynamic }    // I SSlice
};
static const PWelsSliceHeaderWriteFunc g_pWelsWriteSliceHeader[2] = {  // 0: for base; 1: for ext;
  WelsSliceHeaderWrite,
  WelsSliceHeaderExtWrite
};

//Allocate slice's MB cache buffer
int32_t AllocMbCacheAligned (SMbCache* pMbCache, CMemoryAlign* pMa) {
  pMbCache->pCoeffLevel = (int16_t*)pMa->WelsMallocz (MB_COEFF_LIST_SIZE * sizeof (int16_t), "pMbCache->pCoeffLevel");
  WELS_VERIFY_RETURN_IF (1, (NULL == pMbCache->pCoeffLevel));
  pMbCache->pMemPredMb = (uint8_t*)pMa->WelsMallocz (2 * 256 * sizeof (uint8_t), "pMbCache->pMemPredMb");
  WELS_VERIFY_RETURN_IF (1, (NULL == pMbCache->pMemPredMb));
  pMbCache->pSkipMb = (uint8_t*)pMa->WelsMallocz (384 * sizeof (uint8_t), "pMbCache->pSkipMb");
  WELS_VERIFY_RETURN_IF (1, (NULL == pMbCache->pSkipMb));
  pMbCache->pMemPredBlk4 = (uint8_t*)pMa->WelsMallocz (2 * 16 * sizeof (uint8_t), "pMbCache->pMemPredBlk4");
  WELS_VERIFY_RETURN_IF (1, (NULL == pMbCache->pMemPredBlk4));
  pMbCache->pBufferInterPredMe = (uint8_t*)pMa->WelsMallocz (4 * 640 * sizeof (uint8_t), "pMbCache->pBufferInterPredMe");
  WELS_VERIFY_RETURN_IF (1, (NULL == pMbCache->pBufferInterPredMe));
  pMbCache->pPrevIntra4x4PredModeFlag = (bool*)pMa->WelsMallocz (16 * sizeof (bool),
                                        "pMbCache->pPrevIntra4x4PredModeFlag");
  WELS_VERIFY_RETURN_IF (1, (NULL == pMbCache->pPrevIntra4x4PredModeFlag));
  pMbCache->pRemIntra4x4PredModeFlag = (int8_t*)pMa->WelsMallocz (16 * sizeof (int8_t),
                                       "pMbCache->pRemIntra4x4PredModeFlag");
  WELS_VERIFY_RETURN_IF (1, (NULL == pMbCache->pRemIntra4x4PredModeFlag));
  pMbCache->pDct = (SDCTCoeff*)pMa->WelsMallocz (sizeof (SDCTCoeff), "pMbCache->pDct");
  WELS_VERIFY_RETURN_IF (1, (NULL == pMbCache->pDct));

  return 0;
}

//  Free slice's MB cache buffer
void FreeMbCache (SMbCache* pMbCache, CMemoryAlign* pMa) {
  if (NULL != pMbCache->pCoeffLevel) {
    pMa->WelsFree (pMbCache->pCoeffLevel, "pMbCache->pCoeffLevel");
    pMbCache->pCoeffLevel = NULL;
  }
  if (NULL != pMbCache->pMemPredMb) {
    pMa->WelsFree (pMbCache->pMemPredMb, "pMbCache->pMemPredMb");
    pMbCache->pMemPredMb = NULL;
  }
  if (NULL != pMbCache->pSkipMb) {
    pMa->WelsFree (pMbCache->pSkipMb, "pMbCache->pSkipMb");
    pMbCache->pSkipMb = NULL;
  }
  if (NULL != pMbCache->pMemPredBlk4) {
    pMa->WelsFree (pMbCache->pMemPredBlk4, "pMbCache->pMemPredBlk4");
    pMbCache->pMemPredBlk4 = NULL;
  }
  if (NULL != pMbCache->pBufferInterPredMe) {
    pMa->WelsFree (pMbCache->pBufferInterPredMe, "pMbCache->pBufferInterPredMe");
    pMbCache->pBufferInterPredMe = NULL;
  }
  if (NULL != pMbCache->pPrevIntra4x4PredModeFlag) {
    pMa->WelsFree (pMbCache->pPrevIntra4x4PredModeFlag, "pMbCache->pPrevIntra4x4PredModeFlag");
    pMbCache->pPrevIntra4x4PredModeFlag = NULL;
  }
  if (NULL != pMbCache->pRemIntra4x4PredModeFlag) {
    pMa->WelsFree (pMbCache->pRemIntra4x4PredModeFlag, "pMbCache->pRemIntra4x4PredModeFlag");
    pMbCache->pRemIntra4x4PredModeFlag = NULL;
  }
  if (NULL != pMbCache->pDct) {
    pMa->WelsFree (pMbCache->pDct, "pMbCache->pDct");
    pMbCache->pDct = NULL;
  }
}

//Initialize slice's MB info)
int32_t InitSliceMBInfo (SSliceArgument* pSliceArgument,
                         SSlice* pSlice,
                         const int32_t kiMBWidth,
                         const int32_t kiMBHeight) {
  SSliceHeader* pSliceHeader          = &pSlice->sSliceHeaderExt.sSliceHeader;
  const int32_t* kpSlicesAssignList   = (int32_t*) & (pSliceArgument->uiSliceMbNum[0]);
  const int32_t kiCountNumMbInFrame   = kiMBWidth * kiMBHeight;
  const int32_t kiSliceIdx            = pSlice->uiSliceIdx;
  int32_t iFirstMBInSlice             = 0;
  int32_t iMbNumInSlice               = 0;

  if (SM_SINGLE_SLICE == pSliceArgument->uiSliceMode) {
    iFirstMBInSlice = 0;
    iMbNumInSlice   = kiCountNumMbInFrame;

  } else if ((SM_RASTER_SLICE == pSliceArgument->uiSliceMode) && (0 == pSliceArgument->uiSliceMbNum[0])) {
    iFirstMBInSlice = kiSliceIdx * kiMBWidth;
    iMbNumInSlice   = kiMBWidth;
  } else if (SM_RASTER_SLICE  == pSliceArgument->uiSliceMode ||
             SM_FIXEDSLCNUM_SLICE == pSliceArgument->uiSliceMode) {
    int32_t iMbIdx  = 0;
    for (int i = 0; i < kiSliceIdx; i++) {
      iMbIdx += kpSlicesAssignList[i];
    }

    if (iMbIdx >= kiCountNumMbInFrame) {
      return ENC_RETURN_UNEXPECTED;
    }

    iFirstMBInSlice = iMbIdx;
    iMbNumInSlice   = kpSlicesAssignList[kiSliceIdx];

  } else if (SM_SIZELIMITED_SLICE == pSliceArgument->uiSliceMode) {
    iFirstMBInSlice  = 0;
    iMbNumInSlice    = kiCountNumMbInFrame;

  } else { // any else uiSliceMode?
    assert (0);
  }

  pSlice->iCountMbNumInSlice    = iMbNumInSlice;
  pSliceHeader->iFirstMbInSlice = iFirstMBInSlice;

  return ENC_RETURN_SUCCESS;
}

//Allocate slice's MB info buffer
int32_t AllocateSliceMBBuffer (SSlice* pSlice, CMemoryAlign* pMa) {
  if (AllocMbCacheAligned (&pSlice->sMbCacheInfo, pMa)) {
    return ENC_RETURN_MEMALLOCERR;
  }

  return ENC_RETURN_SUCCESS;
}

// Initialize slice bs buffer info
int32_t InitSliceBsBuffer (SSlice* pSlice,
                           SBitStringAux* pBsWrite,
                           bool bIndependenceBsBuffer,
                           const int32_t iMaxSliceBufferSize,
                           CMemoryAlign* pMa) {
  pSlice->sSliceBs.uiSize  = iMaxSliceBufferSize;
  pSlice->sSliceBs.uiBsPos = 0;

  if (bIndependenceBsBuffer) {
    pSlice->pSliceBsa      = &pSlice->sSliceBs.sBsWrite;
    pSlice->sSliceBs.pBs   = (uint8_t*)pMa->WelsMalloc (iMaxSliceBufferSize, "SliceBs");
    if (NULL == pSlice->sSliceBs.pBs) {
      return ENC_RETURN_MEMALLOCERR;
    }
  } else {
    pSlice->pSliceBsa      = pBsWrite;
    pSlice->sSliceBs.pBs   = NULL;
  }
  return ENC_RETURN_SUCCESS;
}

//free slice bs buffer
void FreeSliceBuffer (SSlice*& pSliceList, const int32_t kiMaxSliceNum, CMemoryAlign* pMa, const char* kpTag) {
  if (NULL != pSliceList) {
    int32_t iSliceIdx = 0;
    while (iSliceIdx < kiMaxSliceNum) {
      SSlice* pSlice = &pSliceList[iSliceIdx];
      FreeMbCache (&pSlice->sMbCacheInfo, pMa);

      //slice bs buffer
      if (NULL != pSlice->sSliceBs.pBs) {
        pMa->WelsFree (pSlice->sSliceBs.pBs, "sSliceBs.pBs");
        pSlice->sSliceBs.pBs = NULL;
      }
      ++ iSliceIdx;
    }
    pMa->WelsFree (pSliceList, kpTag);
    pSliceList = NULL;
  }
}

int32_t InitSliceList (sWelsEncCtx* pCtx,
                       SDqLayer* pDqLayer,
                       SSlice*& pSliceList,
                       const int32_t kiMaxSliceNum,
                       const int32_t kiDlayerIndex,
                       CMemoryAlign* pMa) {
  const int32_t kiMBWidth         = pDqLayer->iMbWidth;
  const int32_t kiMBHeight        = pDqLayer->iMbHeight;
  SSliceArgument* pSliceArgument  = & pCtx->pSvcParam->sSpatialLayers[kiDlayerIndex].sSliceArgument;
  int32_t iMaxSliceBufferSize     = (pCtx)->iSliceBufferSize[kiDlayerIndex];
  int32_t iSliceIdx               = 0;
  int32_t iRet                    = 0;

  //SM_SINGLE_SLICE mode using single-thread bs writer pOut->sBsWrite
  //even though multi-thread is on for other layers
  bool bIndependenceBsBuffer   = (pCtx->pSvcParam->iMultipleThreadIdc > 1 &&
                                  SM_SINGLE_SLICE != pSliceArgument->uiSliceMode) ? true : false;

  if (iMaxSliceBufferSize <= 0 || kiMBWidth <= 0 || kiMBHeight <= 0) {
    return ENC_RETURN_UNEXPECTED;
  }

  while (iSliceIdx < kiMaxSliceNum) {
    SSlice* pSlice = pSliceList + iSliceIdx;
    if (NULL == pSlice) {
      return ENC_RETURN_MEMALLOCERR;
    }

    pSlice->uiSliceIdx = iSliceIdx;

    iRet = InitSliceBsBuffer (pSlice,
                              & pCtx->pOut->sBsWrite,
                              bIndependenceBsBuffer,
                              iMaxSliceBufferSize,
                              pMa);
    if (ENC_RETURN_SUCCESS != iRet) {
        return iRet;
    }

    iRet = InitSliceMBInfo (pSliceArgument, pSlice,
                            kiMBWidth, kiMBHeight);

    if (ENC_RETURN_SUCCESS != iRet) {
      return iRet;
    }
    iRet = AllocateSliceMBBuffer (pSlice, pMa);

    if (ENC_RETURN_SUCCESS != iRet) {
      return iRet;
    }
    ++ iSliceIdx;
  }
  return ENC_RETURN_SUCCESS;
}
/*
int32_t InitOneSliceInThread (sWelsEncCtx* pCtx,
                              SSlice*& pSlice,
                              const int32_t kiThreadIdx,
                              const int32_t kiDlayerIdx,
                              const int32_t kiSliceIdx) {
  SDqLayer* pDqLayer                  = pCtx->pCurDqLayer;
  SSliceArgument* pSliceArgument      = & pCtx->pSvcParam->sSpatialLayers[kiDlayerIdx].sSliceArgument;
  const int32_t kiMBWidth             = pDqLayer->iMbWidth;
  const int32_t kiMBHeight            = pDqLayer->iMbHeight;
  const int32_t kiCodedNumInThread    = pDqLayer->sSliceThreadInfo.iEncodedSliceNumInThread[kiThreadIdx];
  const int32_t kiMaxSliceNumInThread = pDqLayer->sSliceThreadInfo.iMaxSliceNumInThread[kiThreadIdx];
  int32_t iRet                        = 0;

  if (kiCodedNumInThread >= kiMaxSliceNumInThread) {
    iRet = ReallocateSliceInThread(pCtx, pDqLayer, kiDlayerIdx, kiThreadIdx);
    if (ENC_RETURN_SUCCESS != iRet) {
      return iRet;
    }
  }

  pSlice = pDqLayer->sSliceThreadInfo.pSliceInThread [kiThreadIdx] + kiCodedNumInThread;
  // Initialize slice bs buffer info
  pSlice->sSliceBs.uiBsPos   = 0;
  pSlice->sSliceBs.iNalIndex = 0;
  pSlice->sSliceBs.pBsBuffer = pCtx->pSliceThreading->pThreadBsBuffer[kiThreadIdx];

  // init slice MB info
  iRet = InitSliceMBInfo (pSliceArgument, pSlice, kiMBWidth, kiMBHeight);
  if (ENC_RETURN_SUCCESS != iRet) {
    return iRet;
  }
  return ENC_RETURN_SUCCESS;
}
*/

int32_t InitSliceThreadInfo (sWelsEncCtx* pCtx,
                             SDqLayer* pDqLayer,
                             const int32_t kiDlayerIndex,
                             CMemoryAlign* pMa) {

  SSliceThreadInfo*  pSliceThreadInfo = &pDqLayer->sSliceThreadInfo;
  int32_t iThreadNum                  = pCtx->pSvcParam->iMultipleThreadIdc;
  int32_t iMaxSliceNumInThread        = 0;
  int32_t iIdx                        = 0;
  int32_t iRet                        = 0;

  assert (iThreadNum > 0);
  iMaxSliceNumInThread = (pCtx->iMaxSliceCount / iThreadNum + 1) * 2;
  iMaxSliceNumInThread =  WELS_MIN (pCtx->iMaxSliceCount, (int) iMaxSliceNumInThread);

  while (iIdx < iThreadNum) {
    pSliceThreadInfo->iMaxSliceNumInThread[iIdx]     = iMaxSliceNumInThread;
    pSliceThreadInfo->iEncodedSliceNumInThread[iIdx] = 0;
    pSliceThreadInfo->pSliceInThread[iIdx] = (SSlice*)pMa->WelsMallocz (sizeof (SSlice) *
        iMaxSliceNumInThread, "pSliceInThread");
    if (NULL == pSliceThreadInfo->pSliceInThread[iIdx]) {
      WelsLog (& (pCtx->sLogCtx), WELS_LOG_ERROR,
               "CWelsH264SVCEncoder::InitSliceThreadInfo: pSliceThreadInfo->pSliceInThread[iIdx] is NULL");
      return ENC_RETURN_MEMALLOCERR;
    }
    iRet = InitSliceList (pCtx,
                          pDqLayer,
                          pSliceThreadInfo->pSliceInThread[iIdx],
                          iMaxSliceNumInThread,
                          kiDlayerIndex,
                          pMa);
    if (ENC_RETURN_SUCCESS != iRet) {
      return iRet;
    }
    iIdx++;
  }

  for (; iIdx < MAX_THREADS_NUM; iIdx++) {
    pSliceThreadInfo->iMaxSliceNumInThread[iIdx]     = iMaxSliceNumInThread;
    pSliceThreadInfo->iEncodedSliceNumInThread[iIdx] = 0;
    pSliceThreadInfo->pSliceInThread[iIdx]           = NULL;
  }
  return ENC_RETURN_SUCCESS;
}

int32_t InitSliceInLayer (sWelsEncCtx* pCtx,
                          SDqLayer* pDqLayer,
                          const int32_t kiDlayerIndex,
                          CMemoryAlign* pMa)  {
  int32_t iRet                  = 0;
  int32_t iSliceIdx             = 0;
  int32_t iMaxSliceNum          = pDqLayer->iMaxSliceNum;

  pDqLayer->ppSliceInLayer = (SSlice**)pMa->WelsMallocz (sizeof (SSlice*) * iMaxSliceNum, "ppSliceInLayer");
  if (NULL ==  pDqLayer->ppSliceInLayer) {
    WelsLog (& (pCtx->sLogCtx), WELS_LOG_ERROR, "CWelsH264SVCEncoder::InitSliceInLayer() pDqLayer->ppSliceInLayer is NULL");
    return ENC_RETURN_MEMALLOCERR;
  }

  InitSliceThreadInfo (pCtx,
                       pDqLayer,
                       kiDlayerIndex,
                       pMa);
  if (ENC_RETURN_SUCCESS != iRet) {
    return iRet;
  }

  pDqLayer->sLayerInfo.pSliceInLayer = (SSlice*)pMa->WelsMallocz (sizeof (SSlice) * iMaxSliceNum, "pSliceInLayer");
  if (NULL ==  pDqLayer->sLayerInfo.pSliceInLayer) {
    WelsLog (& (pCtx->sLogCtx), WELS_LOG_ERROR,
             "CWelsH264SVCEncoder::InitSliceInLayer: pDqLayer->sLayerInfo.pSliceInLayeris NULL");
    return ENC_RETURN_MEMALLOCERR;
  }

  iRet = InitSliceList (pCtx,
                        pDqLayer,
                        pDqLayer->sLayerInfo.pSliceInLayer,
                        iMaxSliceNum,
                        kiDlayerIndex,
                        pMa);
  if (ENC_RETURN_SUCCESS != iRet) {
    return iRet;
  }

  for (iSliceIdx = 0; iSliceIdx < iMaxSliceNum; iSliceIdx++) {
    pDqLayer->ppSliceInLayer[iSliceIdx] = &pDqLayer->sLayerInfo.pSliceInLayer[iSliceIdx];
  }

  return ENC_RETURN_SUCCESS;
}

static inline int32_t InitSliceHeadWithBase (SSlice* pSlice, SSlice* pBaseSlice, const uint8_t kuiRefCount) {
  SSliceHeaderExt* pBaseSHExt  = &pBaseSlice->sSliceHeaderExt;
  SSliceHeaderExt* pSHExt      = &pSlice->sSliceHeaderExt;

  if (NULL == pSlice || NULL == pBaseSlice)
    return ENC_RETURN_INVALIDINPUT;

  pSlice->bSliceHeaderExtFlag  = pBaseSlice->bSliceHeaderExtFlag;
  pSHExt->sSliceHeader.iPpsId     = pBaseSHExt->sSliceHeader.iPpsId;
  pSHExt->sSliceHeader.pPps       = pBaseSHExt->sSliceHeader.pPps;
  pSHExt->sSliceHeader.iSpsId     = pBaseSHExt->sSliceHeader.iSpsId;
  pSHExt->sSliceHeader.pSps       = pBaseSHExt->sSliceHeader.pSps;
  pSHExt->sSliceHeader.uiRefCount = kuiRefCount;
  memcpy (&pSHExt->sSliceHeader.sRefMarking, &pBaseSHExt->sSliceHeader.sRefMarking, sizeof (SRefPicMarking));
  memcpy (&pSHExt->sSliceHeader.sRefReordering, &pBaseSHExt->sSliceHeader.sRefReordering,
          sizeof (SRefPicListReorderSyntax));
  return ENC_RETURN_SUCCESS;

}
static inline int32_t InitSliceRC (SSlice* pSlice, const int32_t kiGlobalQp, const int32_t kiBitsPerMb) {

  if (NULL == pSlice || kiGlobalQp < 0 ||  kiBitsPerMb < 0)
    return ENC_RETURN_INVALIDINPUT;

  pSlice->sSlicingOverRc.iComplexityIndexSlice = 0;
  pSlice->sSlicingOverRc.iCalculatedQpSlice    = kiGlobalQp;
  pSlice->sSlicingOverRc.iTotalQpSlice         = 0;
  pSlice->sSlicingOverRc.iTotalMbSlice         = 0;
  pSlice->sSlicingOverRc.iTargetBitsSlice      = WELS_DIV_ROUND (kiBitsPerMb *
      pSlice->iCountMbNumInSlice,
      INT_MULTIPLY);
  pSlice->sSlicingOverRc.iFrameBitsSlice       = 0;
  pSlice->sSlicingOverRc.iGomBitsSlice         = 0;

  return ENC_RETURN_SUCCESS;
}

int32_t ReallocateSliceList (sWelsEncCtx* pCtx,
                             SSliceArgument* pSliceArgument,
                             SSlice*& pSliceList,
                             const int32_t kiMaxSliceNumOld,
                             const int32_t kiMaxSliceNumNew) {
  CMemoryAlign* pMA           = pCtx->pMemAlign;
  SDqLayer* pCurLayer         = pCtx->pCurDqLayer;
  SSlice* pBaseSlice          = NULL;
  SSlice* pNewSliceList       = NULL;
  SSlice* pSlice              = NULL;
  int32_t iSliceIdx           = 0;
  int32_t iRet                = 0;
  const int32_t kiCurDid      = pCtx->uiDependencyId;
  int32_t iMaxSliceBufferSize = (pCtx)->iSliceBufferSize[kiCurDid];
  int32_t iBitsPerMb          = WELS_DIV_ROUND (pCtx->pWelsSvcRc[kiCurDid].iTargetBits * INT_MULTIPLY,
                                pCtx->pWelsSvcRc[kiCurDid].iNumberMbFrame);
  bool bIndependenceBsBuffer  = (pCtx->pSvcParam->iMultipleThreadIdc > 1 &&
                                 SM_SINGLE_SLICE != pSliceArgument->uiSliceMode) ? true : false;

  if (NULL == pSliceList || NULL == pSliceArgument) {
    return ENC_RETURN_INVALIDINPUT;
  }

  pNewSliceList = (SSlice*)pMA->WelsMallocz (sizeof (SSlice) * kiMaxSliceNumNew, "Slice");
  if (NULL == pNewSliceList) {
    WelsLog (& (pCtx->sLogCtx), WELS_LOG_ERROR, "CWelsH264SVCEncoder::ReallocateSliceList: pNewSliceList is NULL");
    return ENC_RETURN_MEMALLOCERR;
  }

  memcpy (pNewSliceList, pSliceList, sizeof (SSlice) * kiMaxSliceNumOld);
  iSliceIdx   = kiMaxSliceNumOld;
  pBaseSlice  = &pSliceList[0];

  for (; iSliceIdx < kiMaxSliceNumNew; iSliceIdx++) {
    pSlice = pNewSliceList + iSliceIdx;
    if (NULL == pSlice) {
      FreeSliceBuffer(pNewSliceList, kiMaxSliceNumNew, pMA, "ReallocateSliceList()::InitSliceBsBuffer()");
      return ENC_RETURN_MEMALLOCERR;
    }

    pSlice->uiSliceIdx = iSliceIdx;

    iRet = InitSliceBsBuffer (pSlice,
                              & pCtx->pOut->sBsWrite,
                              bIndependenceBsBuffer,
                              iMaxSliceBufferSize,
                              pMA);
    if (ENC_RETURN_SUCCESS != iRet) {
      FreeSliceBuffer(pNewSliceList, kiMaxSliceNumNew, pMA, "ReallocateSliceList()::InitSliceBsBuffer()");
      return iRet;
    }

    iRet = AllocateSliceMBBuffer (pSlice, pMA);
    if (ENC_RETURN_SUCCESS != iRet) {
      FreeSliceBuffer(pNewSliceList, kiMaxSliceNumNew, pMA, "ReallocateSliceList()::InitSliceBsBuffer()");
      return iRet;
    }

    iRet = InitSliceMBInfo (pSliceArgument,
                            pSlice,
                            pCurLayer->iMbWidth,
                            pCurLayer->iMbHeight);
    if (ENC_RETURN_SUCCESS != iRet) {
      FreeSliceBuffer(pNewSliceList, kiMaxSliceNumNew, pMA, "ReallocateSliceList()::InitSliceBsBuffer()");
      return iRet;
    }

    iRet = InitSliceHeadWithBase (pSlice, pBaseSlice, pCtx->iNumRef0);
    if (ENC_RETURN_SUCCESS != iRet) {
      FreeSliceBuffer(pNewSliceList, kiMaxSliceNumNew, pMA, "ReallocateSliceList()::InitSliceBsBuffer()");
      return iRet;
    }

    iRet = InitSliceRC (pSlice, pCtx->iGlobalQp, iBitsPerMb);
    if (ENC_RETURN_SUCCESS != iRet) {
      FreeSliceBuffer(pNewSliceList, kiMaxSliceNumNew, pMA, "ReallocateSliceList()::InitSliceBsBuffer()");
      return iRet;
    }
  }

  pMA->WelsFree (pSliceList, "Slice");
  pSliceList = pNewSliceList;

  return ENC_RETURN_SUCCESS;

}

int32_t CalculateNewSliceNum (SDqLayer* pDqLayer,
                              SSlice* pLastCodedSlice,
                              const int32_t iMaxSliceNumOld,
                              int32_t& iMaxSliceNumNew) {
  int32_t CodedMBNum = 0;

  if (NULL == pLastCodedSlice)
    return ENC_RETURN_INVALIDINPUT;

  CodedMBNum = pLastCodedSlice->sSliceHeaderExt.sSliceHeader.iFirstMbInSlice +
               pLastCodedSlice->iCountMbNumInSlice;
  if (CodedMBNum <= 0) {
    return ENC_RETURN_UNEXPECTED;
  }
  //iMaxSliceNumNew = iMaxSliceNumOld * (pDqLayer->iMbWidth * pDqLayer->iMbHeight / CodedMBNum + 1);
  //TO DO, will used above logic later, here keep origin logic in order to pass ut
  iMaxSliceNumNew = iMaxSliceNumOld;
  iMaxSliceNumNew *= SLICE_NUM_EXPAND_COEF;
  return ENC_RETURN_SUCCESS;
}

/*
int32_t ReallocateSliceInThread (sWelsEncCtx* pCtx,
                                 SDqLayer* pDqLayer,
                                 const int32_t kiDlayerIdx,
                                 const int32_t kiThreadIndex) {

  int32_t iMaxSliceNumInThread   = pDqLayer->sSliceThreadInfo.iMaxSliceNumInThread[kiThreadIndex];
  int32_t iMaxSliceNumUpdate     = 0;
  int32_t iRet                   = 0;
  SSlice* pLastCodedSlice        = pDqLayer->sSliceThreadInfo.pSliceInThread[kiThreadIndex] + (iMaxSliceNumInThread - 1);
  SSliceArgument* pSliceArgument = & pCtx->pSvcParam->sSpatialLayers[kiDlayerIdx].sSliceArgument;

  iRet = CalculateNewSliceNum (pDqLayer,
                               pLastCodedSlice,
                               iMaxSliceNumInThread,
                               iMaxSliceNumUpdate);

  if (ENC_RETURN_SUCCESS != iRet) {
    return iRet;
  }

  iRet = ReallocateSliceList (pCtx,
                              pSliceArgument,
                              pDqLayer->sSliceThreadInfo.pSliceInThread[kiThreadIndex],
                              iMaxSliceNumInThread,
                              iMaxSliceNumUpdate);
  if (ENC_RETURN_SUCCESS != iRet) {
    return iRet;
  }

  return ENC_RETURN_SUCCESS;
}
*/

int32_t ReallocSliceBuffer (sWelsEncCtx* pCtx) {

  CMemoryAlign* pMA        = pCtx->pMemAlign;
  SDqLayer* pCurLayer      = pCtx->pCurDqLayer;
  SSlice** ppSlice         = NULL;
  int32_t iMaxSliceNumOld  = pCurLayer->sSliceEncCtx.iMaxSliceNumConstraint;
  int32_t iMaxSliceNumNew  = 0;
  int32_t iRet             = 0;
  int32_t iSliceIdx        = 0;
  const int32_t kiCurDid   = pCtx->uiDependencyId;

  SSlice* pLastCodedSlice        = pCurLayer->sLayerInfo.pSliceInLayer + (iMaxSliceNumOld - 1);
  SSliceArgument* pSliceArgument = & pCtx->pSvcParam->sSpatialLayers[kiCurDid].sSliceArgument;
  iRet = CalculateNewSliceNum (pCurLayer,
                               pLastCodedSlice,
                               iMaxSliceNumOld,
                               iMaxSliceNumNew);

  if (ENC_RETURN_SUCCESS != iRet) {
    return iRet;
  }

  iRet = ReallocateSliceList (pCtx,
                              pSliceArgument,
                              pCurLayer->sLayerInfo.pSliceInLayer,
                              iMaxSliceNumOld,
                              iMaxSliceNumNew);
  if (ENC_RETURN_SUCCESS != iRet) {
    return iRet;
  }

  // update for ppsliceInlayer
  ppSlice = (SSlice**)pMA->WelsMallocz (sizeof (SSlice*) * iMaxSliceNumNew, "ppSlice");
  if (NULL == ppSlice) {
    WelsLog (& (pCtx->sLogCtx), WELS_LOG_ERROR, "CWelsH264SVCEncoder::ReallocSliceBuffer: ppSlice is NULL");
    return ENC_RETURN_MEMALLOCERR;
  }
  pMA->WelsFree (pCurLayer->ppSliceInLayer, "ppSliceInLayer");
  pCurLayer->ppSliceInLayer = ppSlice;

  for (iSliceIdx = 0; iSliceIdx < iMaxSliceNumNew; iSliceIdx++) {
    pCurLayer->ppSliceInLayer[iSliceIdx] = &pCurLayer->sLayerInfo.pSliceInLayer[iSliceIdx];
  }

  if (pCtx->iMaxSliceCount < iMaxSliceNumNew) {
    pCtx->iMaxSliceCount = iMaxSliceNumNew;
  }

  pCurLayer->sSliceEncCtx.iMaxSliceNumConstraint = iMaxSliceNumNew;
  pCurLayer->iMaxSliceNum = iMaxSliceNumNew;
  return ENC_RETURN_SUCCESS;
}

/*
int32_t ReOrderSliceInLayer (SDqLayer* pCurLayer,
                             const int32_t kiThreadNum,
                             const int32_t kiPartitionNum) {
  SSlice* pSliceInThread    = NULL;
  int32_t iThreadIdx        = 0;
  int32_t iPartitionIdx     = 0;
  int32_t iPartitionID      = 0;
  int32_t iSliceIdx         = 0;
  int32_t iSliceNumInThread = 0;
  int32_t iPartitionOffset  = 0;
  int32_t iActualSliceIdx   = 0;
  int32_t aiPartitionOffset[MAX_THREADS_NUM] = {0};

  //for non-dynamic slice mode, kiPartitionNum = 1, iPartitionOffset = 0
  for(iPartitionIdx = 0; iPartitionIdx < kiPartitionNum; iPartitionIdx++) {
    aiPartitionOffset[iPartitionIdx] = iPartitionOffset;
    iPartitionOffset                += pCurLayer->pNumSliceCodedOfPartition[iPartitionIdx];
  }

  for (iThreadIdx = 0; iThreadIdx < kiThreadNum; iThreadIdx++) {
    iSliceNumInThread = pCurLayer->sSliceThreadInfo.iEncodedSliceNumInThread[iThreadIdx];

    for(iSliceIdx =0; iSliceIdx < iSliceNumInThread; iSliceIdx++) {
      pSliceInThread = pCurLayer->sSliceThreadInfo.pSliceInThread[iThreadIdx] + iSliceIdx;
      if (NULL == pSliceInThread) {
        return ENC_RETURN_UNEXPECTED;
      }

      iPartitionID    = pSliceInThread->uiSliceIdx % kiPartitionNum;
      iActualSliceIdx = aiPartitionOffset[iPartitionID] + pSliceInThread->uiSliceIdx / kiPartitionNum;
      pCurLayer->ppSliceInLayer[iActualSliceIdx] = pSliceInThread;
    }
  }

  return ENC_RETURN_SUCCESS;
}

static inline int32_t CheckAllSliceBuffer(SDqLayer* pCurLayer, const int32_t kiCodedSliceNum) {
  int32_t iSliceIdx = 0;
  for(; iSliceIdx <kiCodedSliceNum ; iSliceIdx ++ ) {
    if ( NULL == pCurLayer->ppSliceInLayer[iSliceIdx]) {
      return ENC_RETURN_UNEXPECTED;
    }

    if ( iSliceIdx != pCurLayer->ppSliceInLayer[iSliceIdx]->uiSliceIdx) {
      return ENC_RETURN_UNEXPECTED;
    }
  }

  return ENC_RETURN_SUCCESS;
}

int32_t SliceLayerInfoUpdate (sWelsEncCtx* pCtx, const int32_t kiDlayerIndex) {

  CMemoryAlign* pMA       = pCtx->pMemAlign;
  SDqLayer* pCurLayer     = pCtx->pCurDqLayer;
  SSlice** ppSlice        = NULL;
  int32_t iCodedSliceNum  = 0;
  int32_t iThreadIdx      = 0;
  int32_t iRet            = 0;
  SSliceArgument* pSliceArgument = & pCtx->pSvcParam->sSpatialLayers[kiDlayerIndex].sSliceArgument;
  int32_t iPartitionNum   = (SM_SIZELIMITED_SLICE == pSliceArgument->uiSliceMode) ? pCtx->iActiveThreadsNum : 1;

  for ( ; iThreadIdx < pCtx->iActiveThreadsNum; iThreadIdx++) {
    iCodedSliceNum += pCurLayer->sSliceThreadInfo.iMaxSliceNumInThread[iThreadIdx];
  }

  if (iCodedSliceNum <= 0) {
    return ENC_RETURN_UNEXPECTED;
  }

  //reallocate ppSliceInLayer if total encoded slice num exceed max slice num
  if (iCodedSliceNum > pCurLayer->sSliceEncCtx.iMaxSliceNumConstraint) {
    ppSlice = (SSlice**)pMA->WelsMallocz (sizeof (SSlice*) * iCodedSliceNum, "ppSlice");
    if (NULL == ppSlice) {
      WelsLog (& (pCtx->sLogCtx), WELS_LOG_ERROR, "CWelsH264SVCEncoder::SliceLayerInfoUpdate: ppSlice is NULL");
      return ENC_RETURN_MEMALLOCERR;
    }

    pMA->WelsFree (pCurLayer->ppSliceInLayer, "ppSliceInLayer");
    pCurLayer->ppSliceInLayer = ppSlice;
    pCurLayer->sSliceEncCtx.iMaxSliceNumConstraint = iCodedSliceNum;
  }

  //update ppSliceInLayer based on pSliceInThread, reordering based on slice index
  iRet = ReOrderSliceInLayer (pCurLayer, pCtx->iActiveThreadsNum, iPartitionNum);
  if (ENC_RETURN_SUCCESS != iRet) {
    WelsLog (& (pCtx->sLogCtx), WELS_LOG_ERROR,
             "CWelsH264SVCEncoder::SliceLayerInfoUpdate: ReOrderSliceInLayer failed");
    return iRet;
  }

  iRet = CheckAllSliceBuffer(pCurLayer, pCtx->iActiveThreadsNum);
  if (ENC_RETURN_SUCCESS != iRet) {
    WelsLog (& (pCtx->sLogCtx), WELS_LOG_ERROR,
             "CWelsH264SVCEncoder::SliceLayerInfoUpdate: ReOrderSliceInLayerDynamic failed");
    return iRet;
  }

  return ENC_RETURN_SUCCESS;
}
*/

int32_t WelsCodeOneSlice (sWelsEncCtx* pEncCtx, const int32_t kiSliceIdx, const int32_t kiNalType) {
  SDqLayer* pCurLayer                   = pEncCtx->pCurDqLayer;
  SNalUnitHeaderExt* pNalHeadExt        = &pCurLayer->sLayerInfo.sNalHeaderExt;
  SSlice* pCurSlice                     = pCurLayer->ppSliceInLayer[kiSliceIdx];
  SBitStringAux* pBs                    = pCurSlice->pSliceBsa;
  const int32_t kiDynamicSliceFlag      = (pEncCtx->pSvcParam->sSpatialLayers[pEncCtx->uiDependencyId].sSliceArgument.uiSliceMode
                                          == SM_SIZELIMITED_SLICE);

  assert (kiSliceIdx == (int) pCurSlice->uiSliceIdx);

  if (I_SLICE == pEncCtx->eSliceType) {
    pNalHeadExt->bIdrFlag = 1;
    pCurSlice->sScaleShift = 0;
  } else {
    const uint32_t kuiTemporalId = pNalHeadExt->uiTemporalId;
    pCurSlice->sScaleShift = kuiTemporalId ? (kuiTemporalId - pEncCtx->pRefPic->uiTemporalId) : 0;
  }

  WelsSliceHeaderExtInit (pEncCtx, pCurLayer, pCurSlice);

  g_pWelsWriteSliceHeader[pCurSlice->bSliceHeaderExtFlag] (pEncCtx, pBs, pCurLayer, pCurSlice,
      pEncCtx->pFuncList->pParametersetStrategy);

  pCurSlice->uiLastMbQp = pCurLayer->sLayerInfo.pPpsP->iPicInitQp + pCurSlice->sSliceHeaderExt.sSliceHeader.iSliceQpDelta;

  int32_t iEncReturn = g_pWelsSliceCoding[pNalHeadExt->bIdrFlag][kiDynamicSliceFlag] (pEncCtx, pCurSlice);
  if (ENC_RETURN_SUCCESS != iEncReturn)
    return iEncReturn;

  WelsWriteSliceEndSyn (pCurSlice, pEncCtx->pSvcParam->iEntropyCodingModeFlag != 0);

  return ENC_RETURN_SUCCESS;
}

//pFunc: UpdateMbNeighbourInfoForNextSlice()
void UpdateMbNeighbourInfoForNextSlice (SDqLayer* pCurDq,
                                        SMB* pMbList,
                                        const int32_t kiFirstMbIdxOfNextSlice,
                                        const int32_t kiLastMbIdxInPartition) {
  SSliceCtx* pSliceCtx          = &pCurDq->sSliceEncCtx;
  const int32_t kiMbWidth       = pSliceCtx->iMbWidth;
  int32_t iIdx                  = kiFirstMbIdxOfNextSlice;
  int32_t iNextSliceFirstMbIdxRowStart = ((kiFirstMbIdxOfNextSlice % kiMbWidth) ? 1 : 0);
  int32_t iCountMbUpdate        = kiMbWidth +
                                  iNextSliceFirstMbIdxRowStart; //need to update MB(iMbXY+1) to MB(iMbXY+1+row) in common case
  const int32_t kiEndMbNeedUpdate       = kiFirstMbIdxOfNextSlice + iCountMbUpdate;
  SMB* pMb = &pMbList[iIdx];

  do {
    UpdateMbNeighbor (pCurDq, pMb, kiMbWidth, WelsMbToSliceIdc (pCurDq, pMb->iMbXY));
    ++ pMb;
    ++ iIdx;
  } while ((iIdx < kiEndMbNeedUpdate) &&
           (iIdx <= kiLastMbIdxInPartition));
}


void AddSliceBoundary (sWelsEncCtx* pEncCtx, SSlice* pCurSlice, SSliceCtx* pSliceCtx, SMB* pCurMb,
                       int32_t iFirstMbIdxOfNextSlice, const int32_t kiLastMbIdxInPartition) {
  SDqLayer*     pCurLayer       = pEncCtx->pCurDqLayer;
  SSlice**      ppSliceInLayer  = pCurLayer->ppSliceInLayer;
  int32_t       iCurMbIdx       = pCurMb->iMbXY;
  uint16_t      iCurSliceIdc    = pSliceCtx->pOverallMbMap[ iCurMbIdx ];
  const int32_t kiSliceIdxStep  = pEncCtx->iActiveThreadsNum;
  uint16_t      iNextSliceIdc   = iCurSliceIdc + kiSliceIdxStep;
  SSlice*       pNextSlice      = NULL;

  SMB* pMbList = pCurLayer->sMbDataP;

  //update cur pSlice info
  pCurSlice->sSliceHeaderExt.uiNumMbsInSlice = 1 + iCurMbIdx - pCurSlice->sSliceHeaderExt.sSliceHeader.iFirstMbInSlice;

  //pNextSlice pointer/initialization
  pNextSlice = pCurLayer->ppSliceInLayer[ iNextSliceIdc ];

#if _DEBUG
  assert (NULL != pNextSlice);
  // now ( pSliceCtx->iSliceNumInFrame < pSliceCtx->iMaxSliceNumConstraint ) always true by the call of this pFunc
#endif

  //init next pSlice info
  pNextSlice->bSliceHeaderExtFlag =
    (NAL_UNIT_CODED_SLICE_EXT == pCurLayer->sLayerInfo.sNalHeaderExt.sNalUnitHeader.eNalUnitType);
  memcpy (&pNextSlice->sSliceHeaderExt, &pCurSlice->sSliceHeaderExt,
          sizeof (SSliceHeaderExt)); // confirmed_safe_unsafe_usage
  ppSliceInLayer[iNextSliceIdc]->sSliceHeaderExt.sSliceHeader.iFirstMbInSlice = iFirstMbIdxOfNextSlice;
  WelsSetMemMultiplebytes_c (pSliceCtx->pOverallMbMap + iFirstMbIdxOfNextSlice, iNextSliceIdc,
                             (kiLastMbIdxInPartition - iFirstMbIdxOfNextSlice + 1), sizeof (uint16_t));

  //DYNAMIC_SLICING_ONE_THREAD: update pMbList slice_neighbor_info
  UpdateMbNeighbourInfoForNextSlice (pCurLayer, pMbList, iFirstMbIdxOfNextSlice, kiLastMbIdxInPartition);
}

bool DynSlcJudgeSliceBoundaryStepBack (void* pCtx, void* pSlice, SSliceCtx* pSliceCtx, SMB* pCurMb,
                                       SDynamicSlicingStack* pDss) {
  sWelsEncCtx* pEncCtx = (sWelsEncCtx*)pCtx;
  SSlice* pCurSlice = (SSlice*)pSlice;
  int32_t iCurMbIdx = pCurMb->iMbXY;
  uint32_t uiLen = 0;
  int32_t iPosBitOffset = 0;
  const int32_t  kiActiveThreadsNum = pEncCtx->iActiveThreadsNum;
  const int32_t  kiPartitaionId = pCurSlice->uiSliceIdx % kiActiveThreadsNum;
  const int32_t  kiLastMbIdxInPartition = pEncCtx->pCurDqLayer->pLastMbIdxOfPartition[kiPartitaionId];

  const bool    kbCurMbNotFirstMbOfCurSlice      = ((iCurMbIdx > 0) && (pSliceCtx->pOverallMbMap[iCurMbIdx] ==
      pSliceCtx->pOverallMbMap[iCurMbIdx - 1]));
  const bool    kbCurMbNotLastMbOfCurPartition = iCurMbIdx < kiLastMbIdxInPartition;

  if (pCurSlice->bDynamicSlicingSliceSizeCtrlFlag)
    return false;

  iPosBitOffset = (pDss->iCurrentPos - pDss->iStartPos);
#if _DEBUG
  assert (iPosBitOffset >= 0);
#endif
  uiLen = ((iPosBitOffset >> 3) + ((iPosBitOffset & 0x07) ? 1 : 0));

  if ((kbCurMbNotFirstMbOfCurSlice
       && JUMPPACKETSIZE_JUDGE (uiLen, iCurMbIdx, pSliceCtx->uiSliceSizeConstraint)) /*jump_avoiding_pack_exceed*/
      && kbCurMbNotLastMbOfCurPartition) { //decide to add new pSlice

    WelsLog (&pEncCtx->sLogCtx, WELS_LOG_DETAIL,
             "DynSlcJudgeSliceBoundaryStepBack: AddSliceBoundary: iCurMbIdx=%d, uiLen=%d, uiSliceIdx=%d", iCurMbIdx, uiLen,
             pCurSlice->uiSliceIdx);

    if (pEncCtx->pSvcParam->iMultipleThreadIdc > 1) {
      WelsMutexLock (&pEncCtx->pSliceThreading->mutexSliceNumUpdate);
      //lock the acessing to this variable: pSliceCtx->iSliceNumInFrame
    }
    const bool    kbSliceNumNotExceedConstraint = pSliceCtx->iSliceNumInFrame <
        pSliceCtx->iMaxSliceNumConstraint; /*tmp choice to avoid complex memory operation, 100520, to be modify*/
    const bool    kbSliceIdxNotExceedConstraint = ((int) pCurSlice->uiSliceIdx + kiActiveThreadsNum) <
        pSliceCtx->iMaxSliceNumConstraint;
    const bool    kbSliceNumReachConstraint = (pSliceCtx->iSliceNumInFrame ==
        pSliceCtx->iMaxSliceNumConstraint);

    //DYNAMIC_SLICING_ONE_THREAD: judge jump_avoiding_pack_exceed
    if (kbSliceNumNotExceedConstraint && kbSliceIdxNotExceedConstraint) {//able to add new pSlice

      AddSliceBoundary (pEncCtx, pCurSlice, pSliceCtx, pCurMb, iCurMbIdx, kiLastMbIdxInPartition);

      ++ pSliceCtx->iSliceNumInFrame;

      if (pEncCtx->pSvcParam->iMultipleThreadIdc > 1) {
        WelsMutexUnlock (&pEncCtx->pSliceThreading->mutexSliceNumUpdate);
      }

      return true;
    }
    if (pEncCtx->pSvcParam->iMultipleThreadIdc > 1) {
      WelsMutexUnlock (&pEncCtx->pSliceThreading->mutexSliceNumUpdate);
    }

    if ((kbSliceNumReachConstraint || !kbSliceIdxNotExceedConstraint)
        && kbCurMbNotLastMbOfCurPartition
        && JUMPPACKETSIZE_JUDGE (uiLen, iCurMbIdx,
                                 pSliceCtx->uiSliceSizeConstraint - ((kiLastMbIdxInPartition - iCurMbIdx) <<
                                     (pCurSlice->uiAssumeLog2BytePerMb) //assume each MB consumes these byte under largest QP
                                                                    ))
       ) {
      // to minimize the impact under the risk of exceeding the size constraint when pSlice num reaches constraint
      pCurSlice->bDynamicSlicingSliceSizeCtrlFlag = true;
    }
  }

  return false;
}

///////////////
//  pMb loop
///////////////
inline void WelsInitInterMDStruc (const SMB* pCurMb, uint16_t* pMvdCostTable, const int32_t kiMvdInterTableStride,
                                  SWelsMD* pMd) {
  pMd->iLambda = g_kiQpCostTable[pCurMb->uiLumaQp];
  pMd->pMvdCost = &pMvdCostTable[pCurMb->uiLumaQp * kiMvdInterTableStride];
  pMd-> iMbPixX = (pCurMb->iMbX << 4);
  pMd-> iMbPixY = (pCurMb->iMbY << 4);
  memset (&pMd->iBlock8x8StaticIdc[0], 0, sizeof (pMd->iBlock8x8StaticIdc));
}
// for inter non-dynamic pSlice
int32_t WelsMdInterMbLoop (sWelsEncCtx* pEncCtx, SSlice* pSlice, void* pWelsMd, const int32_t kiSliceFirstMbXY) {
  SWelsMD* pMd          = (SWelsMD*)pWelsMd;
  SBitStringAux* pBs    = pSlice->pSliceBsa;
  SDqLayer* pCurLayer   = pEncCtx->pCurDqLayer;
  SMbCache* pMbCache    = &pSlice->sMbCacheInfo;
  SMB* pMbList          = pCurLayer->sMbDataP;
  SMB* pCurMb           = NULL;
  int32_t iNumMbCoded   = 0;
  int32_t iNextMbIdx    = kiSliceFirstMbXY;
  int32_t iCurMbIdx     = -1;
  const int32_t kiTotalNumMb = pCurLayer->iMbWidth * pCurLayer->iMbHeight;
  const int32_t kiMvdInterTableStride = pEncCtx->iMvdCostTableStride;
  uint16_t* pMvdCostTable = &pEncCtx->pMvdCostTable[pEncCtx->iMvdCostTableSize];
  const int32_t kiSliceIdx = pSlice->uiSliceIdx;
  const uint8_t kuiChromaQpIndexOffset = pCurLayer->sLayerInfo.pPpsP->uiChromaQpIndexOffset;
  int32_t iEncReturn = ENC_RETURN_SUCCESS;
  SDynamicSlicingStack sDss;
  if (pEncCtx->pSvcParam->iEntropyCodingModeFlag) {
    WelsInitSliceCabac (pEncCtx, pSlice);
    sDss.pRestoreBuffer = NULL;
    sDss.iStartPos = sDss.iCurrentPos = 0;
  }
  pSlice->iMbSkipRun = 0;
  for (;;) {
    if (!pEncCtx->pSvcParam->iEntropyCodingModeFlag)
      pEncCtx->pFuncList->pfStashMBStatus (&sDss, pSlice, pSlice->iMbSkipRun);
    //point to current pMb
    iCurMbIdx = iNextMbIdx;
    pCurMb = &pMbList[ iCurMbIdx ];


    //step(1): set QP for the current MB
    pEncCtx->pFuncList->pfRc.pfWelsRcMbInit (pEncCtx, pCurMb, pSlice);

    //step (2). save some vale for future use, initial pWelsMd
    WelsMdIntraInit (pEncCtx, pCurMb, pMbCache, kiSliceFirstMbXY);
    WelsMdInterInit (pEncCtx, pSlice, pCurMb, kiSliceFirstMbXY);

TRY_REENCODING:
    WelsInitInterMDStruc (pCurMb, pMvdCostTable, kiMvdInterTableStride, pMd);
    pEncCtx->pFuncList->pfInterMd (pEncCtx, pMd, pSlice, pCurMb, pMbCache);
    //mb_qp

    //step (4): save from the MD process from future use
    WelsMdInterSaveSadAndRefMbType ((pCurLayer->pDecPic->uiRefMbType), pMbCache, pCurMb, pMd);

    pEncCtx->pFuncList->pfInterMdBackgroundInfoUpdate (pCurLayer, pCurMb, pMbCache->bCollocatedPredFlag,
        pEncCtx->pRefPic->iPictureType);

    //step (5): update cache
    UpdateNonZeroCountCache (pCurMb, pMbCache);

    //step (6): begin to write bit stream; if the pSlice size is controlled, the writing may be skipped

    iEncReturn = pEncCtx->pFuncList->pfWelsSpatialWriteMbSyn (pEncCtx, pSlice, pCurMb);
    if (!pEncCtx->pSvcParam->iEntropyCodingModeFlag) {
      if (iEncReturn == ENC_RETURN_VLCOVERFLOWFOUND && (pCurMb->uiLumaQp < 50)) {
        pSlice->iMbSkipRun = pEncCtx->pFuncList->pfStashPopMBStatus (&sDss, pSlice);
        UpdateQpForOverflow (pCurMb, kuiChromaQpIndexOffset);
        goto TRY_REENCODING;
      }
    }
    if (ENC_RETURN_SUCCESS != iEncReturn)
      return iEncReturn;


    //step (7): reconstruct current MB
    pCurMb->uiSliceIdc = kiSliceIdx;
    OutputPMbWithoutConstructCsRsNoCopy (pEncCtx, pCurLayer, pSlice, pCurMb);

#if defined(MB_TYPES_CHECK)
    WelsCountMbType (pEncCtx->sPerInfo.iMbCount, P_SLICE, pCurMb);
#endif//MB_TYPES_CHECK

    //step (8): update status and other parameters
    pEncCtx->pFuncList->pfRc.pfWelsRcMbInfoUpdate (pEncCtx, pCurMb, pMd->iCostLuma, pSlice);

    /*judge if all pMb in cur pSlice has been encoded*/
    ++ iNumMbCoded;
    iNextMbIdx = WelsGetNextMbOfSlice (pCurLayer, iCurMbIdx);
    //whether all of MB in current pSlice encoded or not
    if (iNextMbIdx == -1 || iNextMbIdx >= kiTotalNumMb || iNumMbCoded >= kiTotalNumMb) {
      break;
    }
  }

  if (pSlice->iMbSkipRun) {
    BsWriteUE (pBs, pSlice->iMbSkipRun);
  }

  return iEncReturn;
}

// Only for inter dynamic slicing
int32_t WelsMdInterMbLoopOverDynamicSlice (sWelsEncCtx* pEncCtx, SSlice* pSlice, void* pWelsMd,
    const int32_t kiSliceFirstMbXY) {
  SWelsMD* pMd          = (SWelsMD*)pWelsMd;
  SBitStringAux* pBs    = pSlice->pSliceBsa;
  SDqLayer* pCurLayer   = pEncCtx->pCurDqLayer;
  SSliceCtx* pSliceCtx  = &pCurLayer->sSliceEncCtx;
  SMbCache* pMbCache    = &pSlice->sMbCacheInfo;
  SMB* pMbList          = pCurLayer->sMbDataP;
  SMB* pCurMb           = NULL;
  int32_t iNumMbCoded   = 0;
  const int32_t kiTotalNumMb = pCurLayer->iMbWidth * pCurLayer->iMbHeight;
  int32_t iNextMbIdx = kiSliceFirstMbXY;
  int32_t iCurMbIdx = -1;
  const int32_t kiMvdInterTableStride = pEncCtx->iMvdCostTableStride;
  uint16_t* pMvdCostTable = &pEncCtx->pMvdCostTable[pEncCtx->iMvdCostTableSize];
  const int32_t kiSliceIdx = pSlice->uiSliceIdx;
  const int32_t kiPartitionId = (kiSliceIdx % pEncCtx->iActiveThreadsNum);
  const uint8_t kuiChromaQpIndexOffset = pCurLayer->sLayerInfo.pPpsP->uiChromaQpIndexOffset;
  int32_t iEncReturn = ENC_RETURN_SUCCESS;

  SDynamicSlicingStack sDss;
  if (pEncCtx->pSvcParam->iEntropyCodingModeFlag) {
    WelsInitSliceCabac (pEncCtx, pSlice);
    sDss.iStartPos = sDss.iCurrentPos = 0;
    sDss.pRestoreBuffer = pEncCtx->pDynamicBsBuffer[kiPartitionId];
  } else {
    sDss.iStartPos = BsGetBitsPos (pBs);
  }
  pSlice->iMbSkipRun = 0;
  for (;;) {
    //DYNAMIC_SLICING_ONE_THREAD - MultiD
    //stack pBs pointer
    pEncCtx->pFuncList->pfStashMBStatus (&sDss, pSlice, pSlice->iMbSkipRun);

    //point to current pMb
    iCurMbIdx = iNextMbIdx;
    pCurMb = &pMbList[ iCurMbIdx ];

    //step(1): set QP for the current MB
    pEncCtx->pFuncList->pfRc.pfWelsRcMbInit (pEncCtx, pCurMb, pSlice);
    // if already reaches the largest number of slices, set QPs to the upper bound
    if (pSlice->bDynamicSlicingSliceSizeCtrlFlag) {
      //a clearer logic may be:
      //if there is no need from size control from the pSlice size, the QP will be decided by RC; else it will be set to the max QP
      //    however, there are some parameter updating in the rc_mb_init() function, so it cannot be skipped?
      pCurMb->uiLumaQp = pEncCtx->pWelsSvcRc[pEncCtx->uiDependencyId].iMaxQp;
      pCurMb->uiChromaQp = g_kuiChromaQpTable[CLIP3_QP_0_51 (pCurMb->uiLumaQp + kuiChromaQpIndexOffset)];
    }

    //step (2). save some vale for future use, initial pWelsMd
    WelsMdIntraInit (pEncCtx, pCurMb, pMbCache, kiSliceFirstMbXY);
    WelsMdInterInit (pEncCtx, pSlice, pCurMb, kiSliceFirstMbXY);

TRY_REENCODING:
    WelsInitInterMDStruc (pCurMb, pMvdCostTable, kiMvdInterTableStride, pMd);
    pEncCtx->pFuncList->pfInterMd (pEncCtx, pMd, pSlice, pCurMb, pMbCache);
    //mb_qp

    //step (4): save from the MD process from future use
    WelsMdInterSaveSadAndRefMbType ((pCurLayer->pDecPic->uiRefMbType), pMbCache, pCurMb, pMd);

    pEncCtx->pFuncList->pfInterMdBackgroundInfoUpdate (pCurLayer, pCurMb, pMbCache->bCollocatedPredFlag,
        pEncCtx->pRefPic->iPictureType);

    //step (5): update cache
    UpdateNonZeroCountCache (pCurMb, pMbCache);

    //step (6): begin to write bit stream; if the pSlice size is controlled, the writing may be skipped



    iEncReturn = pEncCtx->pFuncList->pfWelsSpatialWriteMbSyn (pEncCtx, pSlice, pCurMb);
    if (iEncReturn == ENC_RETURN_VLCOVERFLOWFOUND  && (pCurMb->uiLumaQp < 50)) {
      pSlice->iMbSkipRun = pEncCtx->pFuncList->pfStashPopMBStatus (&sDss, pSlice);
      UpdateQpForOverflow (pCurMb, kuiChromaQpIndexOffset);
      goto TRY_REENCODING;
    }
    if (ENC_RETURN_SUCCESS != iEncReturn)
      return iEncReturn;


    //DYNAMIC_SLICING_ONE_THREAD - MultiD
    sDss.iCurrentPos = pEncCtx->pFuncList->pfGetBsPosition (pSlice);
    if (DynSlcJudgeSliceBoundaryStepBack (pEncCtx, pSlice, pSliceCtx, pCurMb, &sDss)) {
      pSlice->iMbSkipRun = pEncCtx->pFuncList->pfStashPopMBStatus (&sDss, pSlice);
      pCurLayer->pLastCodedMbIdxOfPartition[kiPartitionId] = iCurMbIdx -
          1; // update pLastCodedMbIdxOfPartition, need to -1 due to stepping back
      ++ pCurLayer->pNumSliceCodedOfPartition[kiPartitionId];

      break;
    }

    //step (7): reconstruct current MB
    pCurMb->uiSliceIdc = kiSliceIdx;
    OutputPMbWithoutConstructCsRsNoCopy (pEncCtx, pCurLayer, pSlice, pCurMb);

#if defined(MB_TYPES_CHECK)
    WelsCountMbType (pEncCtx->sPerInfo.iMbCount, P_SLICE, pCurMb);
#endif//MB_TYPES_CHECK

    //step (8): update status and other parameters
    pEncCtx->pFuncList->pfRc.pfWelsRcMbInfoUpdate (pEncCtx, pCurMb, pMd->iCostLuma, pSlice);

    /*judge if all pMb in cur pSlice has been encoded*/
    ++ iNumMbCoded;
    iNextMbIdx = WelsGetNextMbOfSlice (pCurLayer, iCurMbIdx);
    //whether all of MB in current pSlice encoded or not
    if (iNextMbIdx == -1 || iNextMbIdx >= kiTotalNumMb || iNumMbCoded >= kiTotalNumMb) {
      pCurLayer->pLastCodedMbIdxOfPartition[kiPartitionId] =
        iCurMbIdx; // update pLastCodedMbIdxOfPartition, finish coding, use pCurMb_idx directly
      break;
    }
  }

  if (pSlice->iMbSkipRun) {
    BsWriteUE (pBs, pSlice->iMbSkipRun);
  }

  return iEncReturn;
}

}//namespace WelsEnc
