/*
* Copyright (c) <2018> Side Effects Software Inc.
* All rights reserved.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions are met:
*
* 1. Redistributions of source code must retain the above copyright notice,
*    this list of conditions and the following disclaimer.
*
* 2. The name of Side Effects Software may not be used to endorse or
*    promote products derived from this software without specific prior
*    written permission.
*
* THIS SOFTWARE IS PROVIDED BY SIDE EFFECTS SOFTWARE "AS IS" AND ANY EXPRESS
* OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
* OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN
* NO EVENT SHALL SIDE EFFECTS SOFTWARE BE LIABLE FOR ANY DIRECT, INDIRECT,
* INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
* LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA,
* OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
* LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
* NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
* EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "HoudiniSplineTranslator.h"

#include "HoudiniApi.h"
#include "HoudiniEngine.h"
#include "HoudiniInput.h"
#include "HoudiniOutput.h"
#include "HoudiniAssetComponent.h"
#include "HoudiniSplineComponent.h"
#include "HoudiniEngineUtils.h"
#include "HoudiniEngineString.h"

#include "HoudiniGeoPartObject.h"
#include "Components/SplineComponent.h"

#include "EditorViewportClient.h"
#include "Engine/Selection.h"

#include "HoudiniEnginePrivatePCH.h"

void
FHoudiniSplineTranslator::ExtractStringPositions(const FString& Positions, TArray<FVector>& OutPositions)
{
	TArray< FString > PointStrings;
	static const TCHAR * PositionSeparators[] =
	{
		TEXT(" "),
		TEXT(","),
	};

	int32 NumCoords = Positions.ParseIntoArray(PointStrings, PositionSeparators, 2);
	OutPositions.SetNum(NumCoords / 3);
	for (int32 OutIndex = 0; OutIndex < OutPositions.Num(); OutIndex++)
	{
		const int32& CoordIndex = OutIndex * 3;
		OutPositions[OutIndex].X = FCString::Atof(*(PointStrings[CoordIndex + 0])) * HAPI_UNREAL_SCALE_FACTOR_POSITION;
		OutPositions[OutIndex].Y = FCString::Atof(*(PointStrings[CoordIndex + 2])) * HAPI_UNREAL_SCALE_FACTOR_POSITION;
		OutPositions[OutIndex].Z = FCString::Atof(*(PointStrings[CoordIndex + 1])) * HAPI_UNREAL_SCALE_FACTOR_POSITION;
	}
}

void
FHoudiniSplineTranslator::ConvertToVectorData(const TArray<float> & InRawData, TArray<FVector>& OutVectorData)
{
	OutVectorData.SetNum(InRawData.Num() / 3);

	for (int32 OutIndex = 0; OutIndex < OutVectorData.Num(); OutIndex++)
	{
		const int32& InIndex = OutIndex * 3;
		OutVectorData[OutIndex].X = InRawData[InIndex + 0] * HAPI_UNREAL_SCALE_FACTOR_POSITION;
		OutVectorData[OutIndex].Y = InRawData[InIndex + 2] * HAPI_UNREAL_SCALE_FACTOR_POSITION;
		OutVectorData[OutIndex].Z = InRawData[InIndex + 1] * HAPI_UNREAL_SCALE_FACTOR_POSITION;
	}
}

void 
FHoudiniSplineTranslator::ConvertToVectorData(const TArray<float> & InRawData, TArray<TArray<FVector>>& OutVectorData, const TArray<int32>& CurveCounts) 
{
	OutVectorData.SetNum(CurveCounts.Num());

	int32 TotalNumPoints = 0;
	for (const int32 & NextCount : CurveCounts)
		TotalNumPoints += NextCount;

	// Do not fill the output array, if the total number of points does not match
	if (InRawData.Num() < TotalNumPoints * 3)
		return;


	int32 Itr = 0;

	for (int32 n = 0; n < CurveCounts.Num(); ++n)
	{
		TArray<FVector> & NextVectorDataArray = OutVectorData[n];
		NextVectorDataArray.SetNumZeroed(CurveCounts[n]);

		for (int32 PtIdx = 0; PtIdx < CurveCounts[n]; ++PtIdx)
		{
			if (Itr + 2 >= InRawData.Num())
				return;

			NextVectorDataArray[PtIdx].X = InRawData[Itr]     * HAPI_UNREAL_SCALE_FACTOR_POSITION;
			NextVectorDataArray[PtIdx].Y = InRawData[Itr + 2] * HAPI_UNREAL_SCALE_FACTOR_POSITION;
			NextVectorDataArray[PtIdx].Z = InRawData[Itr + 1] * HAPI_UNREAL_SCALE_FACTOR_POSITION;

			Itr += 3;
		}
	}
}
void
FHoudiniSplineTranslator::UpdateHoudiniInputCurves(UHoudiniAssetComponent* HAC)
{
	for (UHoudiniInput * NextInput : HAC->Inputs)
		UpdateHoudiniInputCurves(NextInput);
}

void
FHoudiniSplineTranslator::UpdateHoudiniInputCurves(UHoudiniInput* Input)
{
	if (!Input || Input->GetInputType() != EHoudiniInputType::Curve)
		return;

	TArray<UHoudiniInputObject*> *InputObjectArray = Input->GetHoudiniInputObjectArray(EHoudiniInputType::Curve);
	if (!InputObjectArray)
		return;

	for (UHoudiniInputObject * NextInputObject : *InputObjectArray)
	{
		UHoudiniInputHoudiniSplineComponent * HoudiniSplineInput = Cast<UHoudiniInputHoudiniSplineComponent>(NextInputObject);
		if (!HoudiniSplineInput)
			continue;

		UHoudiniSplineComponent * HoudiniSplineComponent = HoudiniSplineInput->GetCurveComponent();
		FHoudiniSplineTranslator::UpdateHoudiniCurve(HoudiniSplineComponent);
	}
}

bool
FHoudiniSplineTranslator::UpdateHoudiniCurve(UHoudiniSplineComponent * HoudiniSplineComponent)
{
	if (!HoudiniSplineComponent || HoudiniSplineComponent->IsPendingKill())
		return false;

	int32 CurveNode_id = HoudiniSplineComponent->GetNodeId();
	if (CurveNode_id < 0)
		return false;

	bool Success = true;
	FString CurvePointsString = FString();
	int32 CurveTypeValue = (int32)EHoudiniCurveType::Bezier;
	int32 CurveMethodValue = (int32)EHoudiniCurveMethod::CVs;
	int32 CurveClosed = 0;

	if (CurveNode_id != -1)
	{
		Success &= FHoudiniEngineUtils::HapiGetParameterDataAsString(
			CurveNode_id, HAPI_UNREAL_PARAM_CURVE_COORDS, TEXT(""),	CurvePointsString);

		Success &= FHoudiniEngineUtils::HapiGetParameterDataAsInteger(
			CurveNode_id, HAPI_UNREAL_PARAM_CURVE_TYPE,	(int32)EHoudiniCurveType::Bezier, CurveTypeValue);

		Success &= FHoudiniEngineUtils::HapiGetParameterDataAsInteger(
			CurveNode_id, HAPI_UNREAL_PARAM_CURVE_METHOD, (int32)EHoudiniCurveMethod::CVs, CurveMethodValue);

		Success &= FHoudiniEngineUtils::HapiGetParameterDataAsInteger(
			CurveNode_id, HAPI_UNREAL_PARAM_CURVE_CLOSED, 1, CurveClosed);
	}

	HoudiniSplineComponent->SetCurveType((EHoudiniCurveType)CurveTypeValue);
	HoudiniSplineComponent->SetCurveMethod((EHoudiniCurveMethod)CurveMethodValue);
	HoudiniSplineComponent->SetClosedCurve(CurveClosed == 1);

	// We need to get the NodeInfo to get the parent id
	HAPI_NodeInfo NodeInfo;
	FHoudiniApi::NodeInfo_Init(&NodeInfo);
	HOUDINI_CHECK_ERROR_RETURN(	FHoudiniApi::GetNodeInfo(
		FHoudiniEngine::Get().GetSession(), CurveNode_id, &NodeInfo), false);

	TArray< float > RefinedCurvePositions;
	HAPI_AttributeInfo AttributeRefinedCurvePositions;
	FHoudiniApi::AttributeInfo_Init(&AttributeRefinedCurvePositions);
	Success &= FHoudiniEngineUtils::HapiGetAttributeDataAsFloat(
		CurveNode_id, 0, HAPI_UNREAL_ATTRIB_POSITION, AttributeRefinedCurvePositions, RefinedCurvePositions);

	// Process coords string and extract positions.
	TArray<FVector> CurvePoints;
	FHoudiniSplineTranslator::ExtractStringPositions(CurvePointsString, CurvePoints);

	TArray<FVector> CurveDisplayPoints;
	FHoudiniSplineTranslator::ConvertToVectorData(RefinedCurvePositions, CurveDisplayPoints);

	// build curve points for editable curves.
	if (HoudiniSplineComponent->CurvePoints.Num() < CurvePoints.Num()) 
	{
		HoudiniSplineComponent->CurvePoints.Empty();
		for (FVector NextPos : CurvePoints) 
		{
			FTransform NextTrans = FTransform::Identity;
			NextTrans.SetLocation(NextPos);
			HoudiniSplineComponent->CurvePoints.Add(NextTrans);
		}
	}

	// Update the display point on the curve
	HoudiniSplineComponent->Construct(CurveDisplayPoints);

	HoudiniSplineComponent->MarkChanged(false);

	return Success;
}


bool 
FHoudiniSplineTranslator::HapiUpdateNodeForHoudiniSplineComponent(UHoudiniSplineComponent* HoudiniSplineComponent) 
{
	if (!HoudiniSplineComponent || HoudiniSplineComponent->IsPendingKill())
		return true;

	TArray<FVector> PositionArray;
	TArray<FQuat> RotationArray;
	TArray<FVector> Scales3dArray;
	TArray<float> UniformScaleArray;
	for (FTransform & NextTransform : HoudiniSplineComponent->CurvePoints)
	{
		PositionArray.Add(NextTransform.GetLocation());
		RotationArray.Add(NextTransform.GetRotation());
		Scales3dArray.Add(NextTransform.GetScale3D());
		UniformScaleArray.Add(1.f);
	}

	HAPI_NodeId CurveNode_id = HoudiniSplineComponent->GetNodeId();
	FTransform ParentTransform = HoudiniSplineComponent->GetComponentTransform();

	FString InputNodeNameString = HoudiniSplineComponent->GetName();
	UHoudiniInputHoudiniSplineComponent* InputObject = Cast<UHoudiniInputHoudiniSplineComponent>(HoudiniSplineComponent->GetOuter());
	if (InputObject)
	{
		UHoudiniInput* Input = Cast<UHoudiniInput>(InputObject->GetOuter());
		if (Input)
		{
			InputNodeNameString = Input->GetNodeBaseName();
		}
	}
	InputNodeNameString += TEXT("_curve");

	bool Success = FHoudiniSplineTranslator::HapiCreateCurveInputNodeForData(
		CurveNode_id,
		InputNodeNameString,
		&PositionArray,
		&RotationArray,
		&Scales3dArray,
		HoudiniSplineComponent->GetCurveType(),
		HoudiniSplineComponent->GetCurveMethod(),
		HoudiniSplineComponent->IsClosedCurve(),
		false,
		ParentTransform);

	HoudiniSplineComponent->SetNodeId(CurveNode_id);
	Success &= UpdateHoudiniCurve(HoudiniSplineComponent);

	return Success;
}

bool 
FHoudiniSplineTranslator::HapiCreateInputNodeForHoudiniSplineComponent(
	const FString& InObjNodeName, UHoudiniSplineComponent* SplineComponent)
{
	if (!SplineComponent || SplineComponent->IsPendingKill())
		return true;
	
	bool Success = HapiUpdateNodeForHoudiniSplineComponent(SplineComponent);

	return Success;
}

bool
FHoudiniSplineTranslator::HapiCreateCurveInputNodeForData(
	HAPI_NodeId& CurveNodeId,
	const FString& InputNodeName,
	TArray<FVector>* Positions,
	TArray<FQuat>* Rotations,
	TArray<FVector>* Scales3d,
	EHoudiniCurveType InCurveType,
	EHoudiniCurveMethod InCurveMethod,	
	const bool& InClosed,
	const bool& InForceClose,
	const FTransform& ParentTransform )
{
#if WITH_EDITOR
	// Positions are required
	if (!Positions)
		return false;

	// We also need a valid host asset and 2 points to make a curve
	int32 NumberOfCVs = Positions->Num();
	if (NumberOfCVs < 2)
		return false;

	// Check if connected asset id is valid, if it is not, we need to create an input asset.
	if (CurveNodeId < 0)
	{
		HAPI_NodeId NodeId = -1;
		// Create the curve SOP Node
		if (!FHoudiniSplineTranslator::HapiCreateCurveInputNode(NodeId, InputNodeName))
			return false;

		// Check if we have a valid id for this new input asset.
		if (!FHoudiniEngineUtils::IsHoudiniNodeValid(NodeId))
			return false;

		// We now have a valid id.
		CurveNodeId = NodeId;	
	}
	else
	{
		// We have to revert the Geo to its original state so we can use the Curve SOP:
		// adding parameters to the Curve SOP locked it, preventing its parameters (type, method, isClosed) from working
		FHoudiniApi::RevertGeo(FHoudiniEngine::Get().GetSession(), CurveNodeId);
	}

	//
	// In order to be able to add rotations and scale attributes to the curve SOP, we need to cook it twice:
	// 
	// - First, we send the positions string to it, and cook it without refinement.
	//   this will allow us to get the proper curve CVs, part attributes and curve info to create the desired curve.
	//
	// - We then need to send back all the info extracted from the curve SOP to it, and add the rotation 
	//   and scale attributes to it. This will lock the curve SOP, and prevent the curve type and method 
	//   parameters from functioning properly (hence why we needed the first cook to set that up)
	//

	// Set the curve type and curve method parameters for the curve node
	int32 CurveTypeValue = (int32)InCurveType;
	FHoudiniApi::SetParmIntValue(
		FHoudiniEngine::Get().GetSession(), CurveNodeId,
		HAPI_UNREAL_PARAM_CURVE_TYPE, 0, CurveTypeValue);

	int32 CurveMethodValue = (int32)InCurveMethod;
	FHoudiniApi::SetParmIntValue(
		FHoudiniEngine::Get().GetSession(), CurveNodeId,
		HAPI_UNREAL_PARAM_CURVE_METHOD, 0, CurveMethodValue);

	int32 CurveClosed = InClosed ? 1 : 0;
	FHoudiniApi::SetParmIntValue(
		FHoudiniEngine::Get().GetSession(), CurveNodeId,
		HAPI_UNREAL_PARAM_CURVE_CLOSED, 0, CurveClosed);

	// Reading the curve parameters
	FHoudiniEngineUtils::HapiGetParameterDataAsInteger(
		CurveNodeId, HAPI_UNREAL_PARAM_CURVE_TYPE, 0, CurveTypeValue);
	FHoudiniEngineUtils::HapiGetParameterDataAsInteger(
		CurveNodeId, HAPI_UNREAL_PARAM_CURVE_METHOD, 0, CurveMethodValue);
	FHoudiniEngineUtils::HapiGetParameterDataAsInteger(
		CurveNodeId, HAPI_UNREAL_PARAM_CURVE_CLOSED, 1, CurveClosed);

	if (InForceClose)
	{
		// We need to update the closed parameter
		FHoudiniApi::SetParmIntValue(
			FHoudiniEngine::Get().GetSession(), CurveNodeId, HAPI_UNREAL_PARAM_CURVE_CLOSED, 0, 1);

		CurveClosed = 1;
	}

	// For closed NURBS (CVs and Breakpoints), we have to close the curve manually, by duplicating its last point
	// in order to be able to set the rotations and scales attributes properly.
	bool bCloseCurveManually = false;
	if (CurveClosed && (CurveTypeValue == HAPI_CURVETYPE_NURBS) && (CurveMethodValue != 2))
	{
		// The curve is not closed anymore
		if (HAPI_RESULT_SUCCESS == FHoudiniApi::SetParmIntValue(
			FHoudiniEngine::Get().GetSession(), CurveNodeId,
			HAPI_UNREAL_PARAM_CURVE_CLOSED, 0, 0))
		{
			bCloseCurveManually = true;

			// Duplicating the first point to the end of the curve
			// This needs to be done before sending the position string
			FVector pos = (*Positions)[0];
			Positions->Add(pos);

			CurveClosed = false;
		}
	}

	// Creating the position string
	FString PositionString = TEXT("");
	FHoudiniSplineTranslator::CreatePositionsString(*Positions, PositionString);

	// Get param id for the PositionString and modify it
	HAPI_ParmId ParmId = -1;
	if (FHoudiniApi::GetParmIdFromName(
		FHoudiniEngine::Get().GetSession(), CurveNodeId,
		HAPI_UNREAL_PARAM_CURVE_COORDS, &ParmId) != HAPI_RESULT_SUCCESS)
	{
		return false;
	}

	std::string ConvertedString = TCHAR_TO_UTF8(*PositionString);
	HOUDINI_CHECK_ERROR_RETURN(FHoudiniApi::SetParmStringValue(
		FHoudiniEngine::Get().GetSession(), CurveNodeId,
		ConvertedString.c_str(), ParmId, 0), false);

	// If we don't want to add rotations or scale attributes to the curve, 
	// we can just cook the node normally and stop here.
	bool bAddRotations = (Rotations != nullptr);
	bool bAddScales3d = (Scales3d != nullptr);
	if (!bAddRotations && !bAddScales3d)
	{
		HOUDINI_CHECK_ERROR_RETURN( FHoudiniApi::CookNode(
			FHoudiniEngine::Get().GetSession(), CurveNodeId, nullptr), false);

		return true;
	}

	// Setting up the first cook, without the curve refinement
	HAPI_CookOptions CookOptions;
	FHoudiniApi::CookOptions_Init(&CookOptions);
	CookOptions.curveRefineLOD = 8.0f;
	CookOptions.clearErrorsAndWarnings = false;
	CookOptions.maxVerticesPerPrimitive = -1;
	CookOptions.splitGeosByGroup = false;
	CookOptions.splitGeosByAttribute = false;
	CookOptions.splitAttrSH = 0;
	CookOptions.handleBoxPartTypes = false;
	CookOptions.handleSpherePartTypes = false;
	CookOptions.packedPrimInstancingMode = HAPI_PACKEDPRIM_INSTANCING_MODE_FLAT;
	CookOptions.refineCurveToLinear = false;

	HOUDINI_CHECK_ERROR_RETURN(FHoudiniApi::CookNode(
		FHoudiniEngine::Get().GetSession(), CurveNodeId, &CookOptions), false);

	//  We can now read back the Part infos from the cooked curve.
	HAPI_PartInfo PartInfos;
	FHoudiniApi::PartInfo_Init(&PartInfos);
	HOUDINI_CHECK_ERROR_RETURN(FHoudiniApi::GetPartInfo(
		FHoudiniEngine::Get().GetSession(), CurveNodeId, 0, &PartInfos), false);

	//
	// Depending on the curve type and method, additionnal control points might have been created.
	// We now have to interpolate the rotations and scale attributes for these.
	//

	// Lambda function that interpolates rotation, scale and uniform scales values
	// between two points using fCoeff as a weight, and insert the interpolated value at nInsertIndex
	auto InterpolateRotScaleUScale = [&](const int32& nIndex1, const int32& nIndex2, const float& fCoeff, const int32& nInsertIndex)
	{
		if (Rotations && Rotations->IsValidIndex(nIndex1) && Rotations->IsValidIndex(nIndex2))
		{
			FQuat interpolation = FQuat::Slerp((*Rotations)[nIndex1], (*Rotations)[nIndex2], fCoeff);
			if (Rotations->IsValidIndex(nInsertIndex))
				Rotations->Insert(interpolation, nInsertIndex);
			else
				Rotations->Add(interpolation);
		}

		if (Scales3d && Scales3d->IsValidIndex(nIndex1) && Scales3d->IsValidIndex(nIndex2))
		{
			FVector interpolation = fCoeff * (*Scales3d)[nIndex1] + (1.0f - fCoeff) * (*Scales3d)[nIndex2];
			if (Scales3d->IsValidIndex(nInsertIndex))
				Scales3d->Insert(interpolation, nInsertIndex);
			else
				Scales3d->Add(interpolation);
		}
	};

	// Lambda function that duplicates rotation and scale values
	// at nIndex and insert/adds it at nInsertIndex
	auto DuplicateRotScale = [&](const int32& nIndex, const int32& nInsertIndex)
	{
		if (Rotations && Rotations->IsValidIndex(nIndex))
		{
			FQuat value = (*Rotations)[nIndex];
			if (Rotations->IsValidIndex(nInsertIndex))
				Rotations->Insert(value, nInsertIndex);
			else
				Rotations->Add(value);
		}

		if (Scales3d && Scales3d->IsValidIndex(nIndex))
		{
			FVector value = (*Scales3d)[nIndex];
			if (Scales3d->IsValidIndex(nInsertIndex))
				Scales3d->Insert(value, nInsertIndex);
			else
				Scales3d->Add(value);
		}
	};

	// Do we want to close the curve by ourselves?
	if (bCloseCurveManually)
	{
		// We need to duplicate the info of the first point to the last
		DuplicateRotScale(0, NumberOfCVs++);

		// We need to update the closed parameter
		FHoudiniApi::SetParmIntValue(
			FHoudiniEngine::Get().GetSession(), CurveNodeId,
			HAPI_UNREAL_PARAM_CURVE_CLOSED, 0, 1);
	}

	// INTERPOLATION
	if (CurveTypeValue == HAPI_CURVETYPE_NURBS)
	{
		// Closed NURBS have additional points reproducing the first ones
		if (InClosed)
		{
			// Only the first one if the method is freehand ... 
			DuplicateRotScale(0, NumberOfCVs++);

			if (CurveMethodValue != 2)
			{
				// ... but also the 2nd and 3rd if the method is CVs or Breakpoints.
				DuplicateRotScale(1, NumberOfCVs++);
				DuplicateRotScale(2, NumberOfCVs++);
			}
		}
		else if (CurveMethodValue == 1)
		{
			// Open NURBS have 2 new points if the method is breakpoint:
			// One between the 1st and 2nd ...
			InterpolateRotScaleUScale(0, 1, 0.5f, 1);

			// ... and one before the last one.
			InterpolateRotScaleUScale(NumberOfCVs, NumberOfCVs - 1, 0.5f, NumberOfCVs);
			NumberOfCVs += 2;
		}
	}
	else if (CurveTypeValue == HAPI_CURVETYPE_BEZIER)
	{
		// Bezier curves requires additional point if the method is Breakpoints
		if (CurveMethodValue == 1)
		{
			// 2 interpolated control points are added per points (except the last one)
			int32 nOffset = 0;
			for (int32 n = 0; n < NumberOfCVs - 1; n++)
			{
				int nIndex1 = n + nOffset;
				int nIndex2 = n + nOffset + 1;

				InterpolateRotScaleUScale(nIndex1, nIndex2, 0.33f, nIndex2);
				nIndex2++;
				InterpolateRotScaleUScale(nIndex1, nIndex2, 0.66f, nIndex2);

				nOffset += 2;
			}
			NumberOfCVs += nOffset;

			if (CurveClosed)
			{
				// If the curve is closed, we need to add 2 points after the last,
				// interpolated between the last and the first one
				int nIndex = NumberOfCVs - 1;
				InterpolateRotScaleUScale(nIndex, 0, 0.33f, NumberOfCVs++);
				InterpolateRotScaleUScale(nIndex, 0, 0.66f, NumberOfCVs++);

				// and finally, the last point is the first..
				DuplicateRotScale(0, NumberOfCVs++);
			}
		}
		else if (CurveClosed)
		{
			// For the other methods, if the bezier curve is closed, the last point is the 1st
			DuplicateRotScale(0, NumberOfCVs++);
		}
	}

	// Even after interpolation, additional points might still be missing:
	// Bezier curves require a certain number of points regarding their order,
	// if points are lacking then HAPI duplicates the last one.
	if (NumberOfCVs < PartInfos.pointCount)
	{
		int nToAdd = PartInfos.pointCount - NumberOfCVs;
		for (int n = 0; n < nToAdd; n++)
		{
			DuplicateRotScale(NumberOfCVs - 1, NumberOfCVs);
			NumberOfCVs++;
		}
	}

	// To avoid crashes, attributes will only be added if we now have the correct number of them
	bAddRotations = bAddRotations && (Rotations->Num() == PartInfos.pointCount);
	bAddScales3d = bAddScales3d && (Scales3d->Num() == PartInfos.pointCount);

	// We need to increase the point attributes count for points in the Part Infos
	HAPI_AttributeOwner NewAttributesOwner = HAPI_ATTROWNER_POINT;
	HAPI_AttributeOwner OriginalAttributesOwner = HAPI_ATTROWNER_POINT;

	int OriginalPointParametersCount = PartInfos.attributeCounts[NewAttributesOwner];
	if (bAddRotations)
		PartInfos.attributeCounts[NewAttributesOwner] += 1;
	if (bAddScales3d)
		PartInfos.attributeCounts[NewAttributesOwner] += 1;

	// Sending the updated PartInfos
	HOUDINI_CHECK_ERROR_RETURN(FHoudiniApi::SetPartInfo(
		FHoudiniEngine::Get().GetSession(),
		CurveNodeId, 0, &PartInfos), false);

	// We need now to reproduce ALL the curves attributes for ALL the Owners..
	for (int nOwner = 0; nOwner < HAPI_ATTROWNER_MAX; nOwner++)
	{
		int nOwnerAttributeCount = nOwner == NewAttributesOwner ? OriginalPointParametersCount : PartInfos.attributeCounts[nOwner];
		if (nOwnerAttributeCount == 0)
			continue;

		TArray<HAPI_StringHandle> AttributeNamesSH;
		AttributeNamesSH.SetNum(nOwnerAttributeCount);
		HOUDINI_CHECK_ERROR_RETURN(FHoudiniApi::GetAttributeNames(
			FHoudiniEngine::Get().GetSession(),
			CurveNodeId, 0,	(HAPI_AttributeOwner)nOwner,
			AttributeNamesSH.GetData(), AttributeNamesSH.Num()), false);

		for (int nAttribute = 0; nAttribute < AttributeNamesSH.Num(); nAttribute++)
		{
			const HAPI_StringHandle sh = AttributeNamesSH[nAttribute];
			if (sh == 0)
				continue;

			// Get the attribute name
			std::string attr_name;
			FHoudiniEngineString::ToStdString(sh, attr_name);
			if (strcmp(attr_name.c_str(), "__topology") == 0)
				continue;

			// and the attribute infos
			HAPI_AttributeInfo attr_info;
			FHoudiniApi::AttributeInfo_Init(&attr_info);
			//FMemory::Memzero< HAPI_AttributeInfo >( attr_info );
			HOUDINI_CHECK_ERROR_RETURN(FHoudiniApi::GetAttributeInfo(
				FHoudiniEngine::Get().GetSession(),
				CurveNodeId, 0,	attr_name.c_str(),
				(HAPI_AttributeOwner)nOwner, &attr_info), false);

			switch (attr_info.storage)
			{
				case HAPI_STORAGETYPE_INT:
				{
					// Storing IntData
					TArray< int > IntData;
					IntData.SetNumUninitialized(attr_info.count * attr_info.tupleSize);

					// GET
					HOUDINI_CHECK_ERROR_RETURN(FHoudiniApi::GetAttributeIntData(
						FHoudiniEngine::Get().GetSession(),
						CurveNodeId, 0,
						attr_name.c_str(), &attr_info, -1,
						IntData.GetData(), 0, attr_info.count), false);

					// ADD
					HOUDINI_CHECK_ERROR_RETURN(FHoudiniApi::AddAttribute(
						FHoudiniEngine::Get().GetSession(),
						CurveNodeId, 0,
						attr_name.c_str(), &attr_info), false);

					// SET
					HOUDINI_CHECK_ERROR_RETURN(FHoudiniApi::SetAttributeIntData(
						FHoudiniEngine::Get().GetSession(),
						CurveNodeId, 0,
						attr_name.c_str(),
						&attr_info, IntData.GetData(),
						0, attr_info.count), false);
				}
				break;

				case HAPI_STORAGETYPE_FLOAT:
				{
					// Storing Float Data
					TArray< float > FloatData;
					FloatData.SetNumUninitialized(attr_info.count * attr_info.tupleSize);

					// GET
					HOUDINI_CHECK_ERROR_RETURN(FHoudiniApi::GetAttributeFloatData(
						FHoudiniEngine::Get().GetSession(),
						CurveNodeId, 0,
						attr_name.c_str(), &attr_info, -1,
						FloatData.GetData(),
						0, attr_info.count), false);

					// ADD
					HOUDINI_CHECK_ERROR_RETURN(FHoudiniApi::AddAttribute(
						FHoudiniEngine::Get().GetSession(),
						CurveNodeId, 0,
						attr_name.c_str(),
						&attr_info), false);

					// SET
					HOUDINI_CHECK_ERROR_RETURN(FHoudiniApi::SetAttributeFloatData(
						FHoudiniEngine::Get().GetSession(),
						CurveNodeId, 0,
						attr_name.c_str(), &attr_info,
						FloatData.GetData(),
						0, attr_info.count), false);
				}
				break;

				case HAPI_STORAGETYPE_STRING:
				{
					// Storing String Data
					TArray<HAPI_StringHandle> StringHandleData;
					StringHandleData.SetNumUninitialized(attr_info.count * attr_info.tupleSize);

					// GET
					HOUDINI_CHECK_ERROR_RETURN(FHoudiniApi::GetAttributeStringData(
						FHoudiniEngine::Get().GetSession(),
						CurveNodeId, 0,
						attr_name.c_str(), &attr_info,
						StringHandleData.GetData(),
						0, attr_info.count), false);

					// Convert the SH to const char *
					TArray<const char *> StringData;
					StringData.SetNumUninitialized(attr_info.count);
					for (int n = 0; n < StringHandleData.Num(); n++)
					{
						// Converting the string
						std::string strSTD;
						FHoudiniEngineString::ToStdString(sh, strSTD);

						StringData[n] = strSTD.c_str();
					}

					// ADD
					HOUDINI_CHECK_ERROR_RETURN(FHoudiniApi::AddAttribute(
						FHoudiniEngine::Get().GetSession(),
						CurveNodeId, 0,
						attr_name.c_str(),
						&attr_info), false);

					// SET
					HOUDINI_CHECK_ERROR_RETURN(FHoudiniApi::SetAttributeStringData(
						FHoudiniEngine::Get().GetSession(),
						CurveNodeId, 0,
						attr_name.c_str(), &attr_info,
						StringData.GetData(),
						0, attr_info.count), false);
				}
				break;

				default:
					continue;
			}
		}
	}

	// Only GET/SET curve infos if the part is a curve...
	// (Closed linear curves are actually not considered as curves...)
	if (PartInfos.type == HAPI_PARTTYPE_CURVE)
	{
		// We need to read the curve infos ...
		HAPI_CurveInfo CurveInfo;
		FHoudiniApi::CurveInfo_Init(&CurveInfo);
		HOUDINI_CHECK_ERROR_RETURN(FHoudiniApi::GetCurveInfo(
			FHoudiniEngine::Get().GetSession(),
			CurveNodeId, 0,
			&CurveInfo), false);

		// ... the curve counts
		TArray< int > CurveCounts;
		CurveCounts.SetNumUninitialized(CurveInfo.curveCount);
		HOUDINI_CHECK_ERROR_RETURN(FHoudiniApi::GetCurveCounts(
			FHoudiniEngine::Get().GetSession(),
			CurveNodeId, 0,
			CurveCounts.GetData(),
			0, CurveInfo.curveCount), false);

		// .. the curve orders
		TArray< int > CurveOrders;
		CurveOrders.SetNumUninitialized(CurveInfo.curveCount);
		HOUDINI_CHECK_ERROR_RETURN(FHoudiniApi::GetCurveOrders(
			FHoudiniEngine::Get().GetSession(),
			CurveNodeId, 0,
			CurveOrders.GetData(),
			0, CurveInfo.curveCount), false);

		// .. And the Knots if they exist.
		TArray< float > KnotsArray;
		if (CurveInfo.hasKnots)
		{
			KnotsArray.SetNumUninitialized(CurveInfo.knotCount);
			HOUDINI_CHECK_ERROR_RETURN(
				FHoudiniApi::GetCurveKnots(
					FHoudiniEngine::Get().GetSession(),
					CurveNodeId, 0,
					KnotsArray.GetData(),
					0, CurveInfo.knotCount), false);
		}

		// To set them back in HAPI
		// CurveInfo
		HOUDINI_CHECK_ERROR_RETURN(FHoudiniApi::SetCurveInfo(
			FHoudiniEngine::Get().GetSession(),
			CurveNodeId, 0,
			&CurveInfo), false);

		// CurveCounts
		HOUDINI_CHECK_ERROR_RETURN(FHoudiniApi::SetCurveCounts(
			FHoudiniEngine::Get().GetSession(),
			CurveNodeId, 0,
			CurveCounts.GetData(),
			0, CurveInfo.curveCount), false);

		// CurveOrders
		HOUDINI_CHECK_ERROR_RETURN(FHoudiniApi::SetCurveOrders(
			FHoudiniEngine::Get().GetSession(),
			CurveNodeId, 0,
			CurveOrders.GetData(),
			0, CurveInfo.curveCount), false);

		// And Knots if they exist
		if (CurveInfo.hasKnots)
		{
			HOUDINI_CHECK_ERROR_RETURN(FHoudiniApi::SetCurveKnots(
				FHoudiniEngine::Get().GetSession(),
				CurveNodeId, 0,
				KnotsArray.GetData(),
				0, CurveInfo.knotCount), false);
		}
	}

	if (PartInfos.faceCount > 0)
	{
		// getting the face counts
		TArray< int > FaceCounts;
		FaceCounts.SetNumUninitialized(PartInfos.faceCount);

		if (FHoudiniApi::GetFaceCounts(
			FHoudiniEngine::Get().GetSession(),
			CurveNodeId, 0,
			FaceCounts.GetData(), 0,
			PartInfos.faceCount) == HAPI_RESULT_SUCCESS)
		{
			// Set the face count
			HOUDINI_CHECK_ERROR_RETURN(FHoudiniApi::SetFaceCounts(
				FHoudiniEngine::Get().GetSession(),
				CurveNodeId, 0,
				FaceCounts.GetData(),
				0, PartInfos.faceCount), false);
		}
	}

	if (PartInfos.vertexCount > 0)
	{
		// the vertex list
		TArray< int > VertexList;
		VertexList.SetNumUninitialized(PartInfos.vertexCount);

		if (FHoudiniApi::GetVertexList(
			FHoudiniEngine::Get().GetSession(),
			CurveNodeId, 0,
			VertexList.GetData(),
			0, PartInfos.vertexCount) == HAPI_RESULT_SUCCESS)
		{
			// setting the vertex list
			HOUDINI_CHECK_ERROR_RETURN(FHoudiniApi::SetVertexList(
				FHoudiniEngine::Get().GetSession(),
				CurveNodeId, 0,
				VertexList.GetData(),
				0, PartInfos.vertexCount), false);
		}
	}

	// We can add attributes to the curve now that all the curves attributes
	// and properties have been reset.
	if (bAddRotations)
	{
		// Create ROTATION attribute info
		HAPI_AttributeInfo AttributeInfoRotation;
		FHoudiniApi::AttributeInfo_Init(&AttributeInfoRotation);
		AttributeInfoRotation.count = NumberOfCVs;
		AttributeInfoRotation.tupleSize = 4;
		AttributeInfoRotation.exists = true;
		AttributeInfoRotation.owner = NewAttributesOwner;
		AttributeInfoRotation.storage = HAPI_STORAGETYPE_FLOAT;
		AttributeInfoRotation.originalOwner = OriginalAttributesOwner;

		HOUDINI_CHECK_ERROR_RETURN(FHoudiniApi::AddAttribute(
			FHoudiniEngine::Get().GetSession(),
			CurveNodeId, 0,
			HAPI_UNREAL_ATTRIB_ROTATION,
			&AttributeInfoRotation), false);

		// Convert the rotation infos
		TArray< float > CurveRotations;
		CurveRotations.SetNumZeroed(NumberOfCVs * 4);
		for (int32 Idx = 0; Idx < NumberOfCVs; ++Idx)
		{
			// Get current quaternion
			const FQuat& RotationQuaternion = (*Rotations)[Idx];

			CurveRotations[Idx * 4 + 0] = RotationQuaternion.X;
			CurveRotations[Idx * 4 + 1] = RotationQuaternion.Z;
			CurveRotations[Idx * 4 + 2] = RotationQuaternion.Y;
			CurveRotations[Idx * 4 + 3] = -RotationQuaternion.W;
		}

		//we can now upload them to our attribute.
		HOUDINI_CHECK_ERROR_RETURN(FHoudiniApi::SetAttributeFloatData(
			FHoudiniEngine::Get().GetSession(),
			CurveNodeId, 0,
			HAPI_UNREAL_ATTRIB_ROTATION,
			&AttributeInfoRotation,
			CurveRotations.GetData(),
			0, AttributeInfoRotation.count), false);
	}

	// Create SCALE attribute info.
	if (bAddScales3d)
	{
		HAPI_AttributeInfo AttributeInfoScale;
		FHoudiniApi::AttributeInfo_Init(&AttributeInfoScale);
		AttributeInfoScale.count = NumberOfCVs;
		AttributeInfoScale.tupleSize = 3;
		AttributeInfoScale.exists = true;
		AttributeInfoScale.owner = NewAttributesOwner;
		AttributeInfoScale.storage = HAPI_STORAGETYPE_FLOAT;
		AttributeInfoScale.originalOwner = OriginalAttributesOwner;

		HOUDINI_CHECK_ERROR_RETURN(FHoudiniApi::AddAttribute(
			FHoudiniEngine::Get().GetSession(),
			CurveNodeId, 0,
			HAPI_UNREAL_ATTRIB_SCALE,
			&AttributeInfoScale), false);

		// Convert the scale
		TArray< float > CurveScales;
		CurveScales.SetNumZeroed(NumberOfCVs * 3);
		for (int32 Idx = 0; Idx < NumberOfCVs; ++Idx)
		{
			// Get current scale
			FVector ScaleVector = (*Scales3d)[Idx];
			CurveScales[Idx * 3 + 0] = ScaleVector.X;
			CurveScales[Idx * 3 + 1] = ScaleVector.Z;
			CurveScales[Idx * 3 + 2] = ScaleVector.Y;
		}

		// We can now upload them to our attribute.
		HOUDINI_CHECK_ERROR_RETURN(FHoudiniApi::SetAttributeFloatData(
			FHoudiniEngine::Get().GetSession(),
			CurveNodeId, 0,
			HAPI_UNREAL_ATTRIB_SCALE,
			&AttributeInfoScale,
			CurveScales.GetData(),
			0, AttributeInfoScale.count), false);
	}

	// Finally, commit the geo ...
	HOUDINI_CHECK_ERROR_RETURN(FHoudiniApi::CommitGeo(
		FHoudiniEngine::Get().GetSession(), CurveNodeId), false);

	CookOptions.refineCurveToLinear = true;
	HOUDINI_CHECK_ERROR_RETURN(FHoudiniApi::CookNode(
		FHoudiniEngine::Get().GetSession(), CurveNodeId, &CookOptions), false);
#endif

	return true;
}

void
FHoudiniSplineTranslator::CreatePositionsString(const TArray<FVector>& InPositions, FString& OutPositionString)
{
	OutPositionString = TEXT("");
	for (int32 Idx = 0; Idx < InPositions.Num(); ++Idx)
	{
		FVector Position = InPositions[Idx];	
		// Convert to meters
		Position /= HAPI_UNREAL_SCALE_FACTOR_POSITION;
		// Swap Y/Z
		OutPositionString += FString::Printf(TEXT("%f, %f, %f "), Position.X, Position.Z, Position.Y);
	}
}

bool
FHoudiniSplineTranslator::HapiCreateCurveInputNode(HAPI_NodeId& OutCurveNodeId, const FString& InputNodeName)
{
	// Create the curve SOP Node
	HAPI_NodeId NewNodeId = -1;
	HOUDINI_CHECK_ERROR_RETURN( FHoudiniEngineUtils::CreateNode(
		-1, TEXT("SOP/curve"), InputNodeName, false, &NewNodeId), false);

	OutCurveNodeId = NewNodeId;

	// Submit default points to curve.
	HAPI_ParmId ParmId = -1;
	HOUDINI_CHECK_ERROR_RETURN(FHoudiniApi::GetParmIdFromName(
		FHoudiniEngine::Get().GetSession(), NewNodeId,
		HAPI_UNREAL_PARAM_CURVE_COORDS, &ParmId), false);

	HOUDINI_CHECK_ERROR_RETURN(FHoudiniApi::SetParmStringValue(
		FHoudiniEngine::Get().GetSession(), NewNodeId,
		HAPI_UNREAL_PARAM_INPUT_CURVE_COORDS_DEFAULT, ParmId, 0), false);

	HOUDINI_CHECK_ERROR_RETURN(FHoudiniApi::CookNode(
		FHoudiniEngine::Get().GetSession(), NewNodeId, nullptr), false);

	return true;
}

UHoudiniSplineComponent* 
FHoudiniSplineTranslator::CreateHoudiniSplineComponentFromHoudiniEditableNode(const int32 & GeoId, const FString & PartName, UHoudiniAssetComponent* OuterHAC) 
{
	if (GeoId < 0)
		return nullptr;

	if (!OuterHAC || OuterHAC->IsPendingKill())
		return nullptr;

	// Create a HoudiniSplineComponent for the editable curve.
	UHoudiniSplineComponent* HoudiniSplineComponent = NewObject<UHoudiniSplineComponent>(
		OuterHAC,
		UHoudiniSplineComponent::StaticClass(), NAME_None, RF_Public | RF_Transactional);

	HoudiniSplineComponent->SetNodeId(GeoId);
	HoudiniSplineComponent->SetGeoPartName(PartName);

	HoudiniSplineComponent->RegisterComponent();
	HoudiniSplineComponent->AttachToComponent(OuterHAC, FAttachmentTransformRules::KeepRelativeTransform);

	UpdateHoudiniCurve(HoudiniSplineComponent);

	ReselectSelectedActors();

	return HoudiniSplineComponent;
	
}

UHoudiniSplineComponent*
FHoudiniSplineTranslator::CreateOutputHoudiniSplineComponent(TArray<FVector>& CurvePoints, const TArray<FVector>& CurveRotations, const TArray<FVector>& CurveScales, UHoudiniAssetComponent* OuterHAC) 
{
	if (!OuterHAC || OuterHAC->IsPendingKill())
		return nullptr;

	UObject* Outer = nullptr;
	if (OuterHAC && !OuterHAC->IsPendingKill())
		Outer = OuterHAC->GetOwner() ? OuterHAC->GetOwner() : OuterHAC->GetOuter();

	UHoudiniSplineComponent *NewHoudiniSplineComponent = NewObject<UHoudiniSplineComponent>(Outer, UHoudiniSplineComponent::StaticClass(), NAME_None, RF_Transactional);

	if (!NewHoudiniSplineComponent)
		return nullptr;

	NewHoudiniSplineComponent->Construct(CurvePoints);

	bool bHasRotations = CurveRotations.Num() == CurvePoints.Num();
	bool bHasScales = CurveScales.Num() == CurvePoints.Num();

	TArray<FTransform> Transforms;
	for (int32 n = 0; n < CurvePoints.Num(); ++n) 
	{
		FTransform NextTransform = FTransform::Identity;
		NextTransform.SetLocation(CurvePoints[n]);
		
		if (bHasRotations)
			NextTransform.SetRotation(CurveRotations[n].Rotation().Quaternion());

		if (bHasScales)
			NextTransform.SetScale3D(CurveScales[n]);

		Transforms.Add(NextTransform);
	}

	NewHoudiniSplineComponent->CurveType = EHoudiniCurveType::Linear;
	NewHoudiniSplineComponent->bIsOutputCurve = true;

	NewHoudiniSplineComponent->AttachToComponent(OuterHAC, FAttachmentTransformRules::KeepRelativeTransform);
	NewHoudiniSplineComponent->RegisterComponent();

	ReselectSelectedActors();
	
	return NewHoudiniSplineComponent;
}

USplineComponent* 
FHoudiniSplineTranslator::CreateOutputUnrealSplineComponent(const TArray<FVector>& CurvePoints, const TArray<FVector>& CurveRotations, const TArray<FVector>& CurveScales, UHoudiniAssetComponent* OuterHAC)
{
	if (!OuterHAC || OuterHAC->IsPendingKill())
		return nullptr;

	UObject* Outer = nullptr;
	if (OuterHAC && !OuterHAC->IsPendingKill())
		Outer = OuterHAC->GetOwner() ? OuterHAC->GetOwner() : OuterHAC->GetOuter();

	USplineComponent* NewSplineComponent = NewObject<USplineComponent>(Outer, USplineComponent::StaticClass(), NAME_None, RF_Transactional);

	if (!NewSplineComponent)
		return nullptr;

	// Clear default USplineComponent's points
	NewSplineComponent->ClearSplinePoints();
	NewSplineComponent->bEditableWhenInherited = false;

	//bool bHasRotations = CurveRotations.Num() == CurvePoints.Num();
	//bool bHasScales = CurveScales.Num() == CurvePoints.Num();
 
	for (int32 n = 0; n < CurvePoints.Num(); ++n) 
	{
		NewSplineComponent->AddSplinePoint(CurvePoints[n], ESplineCoordinateSpace::Local);
		
		//FSplinePoint NewSplinePoint;
		//NewSplinePoint.Position = CurvePoints[n];
		//if (bHasRotations)
		//	NewSplinePoint.Rotation = CurveRotations[n].Rotation();

		//if (bHasScales)
		//	NewSplinePoint.Scale = CurveScales[n];
		//NewSplineComponent->AddPoint(NewSplinePoint, false);
	}

	for (int32 n = 0; n < CurvePoints.Num(); ++n)
		NewSplineComponent->SetSplinePointType(n, ESplinePointType::Linear);

	NewSplineComponent->SetClosedLoop(false);

	/*
	NewSplineComponent->SetClosedLoop(bClosed);

	if (Type == int32(EHoudiniCurveType::Linear))
	{
		for (int32 n = 0; n < CurvePoints.Num(); ++n)
			NewSplineComponent->SetSplinePointType(n, ESplinePointType::Linear);
	}
	else 
	{
		for (int32 n = 0; n < CurvePoints.Num(); ++n)
			NewSplineComponent->SetSplinePointType(n, ESplinePointType::Curve);
	}
	*/

	NewSplineComponent->AttachToComponent(OuterHAC, FAttachmentTransformRules::KeepRelativeTransform);
	NewSplineComponent->RegisterComponent();

	ReselectSelectedActors();

	return NewSplineComponent;
}

bool 
FHoudiniSplineTranslator::UpdateOutputUnrealSplineComponent(const TArray<FVector>& CurvePoints, USplineComponent* EditedSplineComponent, const EHoudiniCurveType& CurveType)
{
	if (!EditedSplineComponent || EditedSplineComponent->IsPendingKill())
		return false;

	if (CurvePoints.Num() < 2)
		return false;

	int MinCount = FMath::Min(CurvePoints.Num(), EditedSplineComponent->GetNumberOfSplinePoints());

	int32 Idx = 0;

	// modify existing points
	for (Idx; Idx < MinCount; ++Idx) 
	{
		if (EditedSplineComponent->GetLocationAtSplinePoint(Idx, ESplineCoordinateSpace::Local) == CurvePoints[Idx])
			continue;

		EditedSplineComponent->RemoveSplinePoint(Idx);
		EditedSplineComponent->AddSplinePoint(CurvePoints[Idx], ESplineCoordinateSpace::Local);	
	}

	// remove extra points
	if (Idx < EditedSplineComponent->GetNumberOfSplinePoints() - 1) 
	{
		for (int32 n = EditedSplineComponent->GetNumberOfSplinePoints() - 1; n >= Idx; --n)
		{
			EditedSplineComponent->RemoveSplinePoint(Idx);
		}
	}

	// append extra points
	for (Idx; Idx < CurvePoints.Num(); ++Idx) 
	{
		EditedSplineComponent->AddSplinePoint(CurvePoints[Idx], ESplineCoordinateSpace::Local);
	}

	//if (EditedSplineComponent->IsClosedLoop() != bool(bClosed))
	//	EditedSplineComponent->SetClosedLoop(bool(bClosed));

	// Set curve type to non-linear
	if (EditedSplineComponent->GetNumberOfSplinePoints() > 0 && 
		EditedSplineComponent->GetSplinePointType(0) == ESplinePointType::Linear && CurveType != EHoudiniCurveType::Linear)
	{
		for (int32 n = 0; n < EditedSplineComponent->GetNumberOfSplinePoints(); ++n)
			EditedSplineComponent->SetSplinePointType(n, ESplinePointType::Curve);
	}

	// Set curve type to linear
	if (EditedSplineComponent->GetNumberOfSplinePoints() > 0 &&
		EditedSplineComponent->GetSplinePointType(0) != ESplinePointType::Linear && CurveType == EHoudiniCurveType::Linear) 
	{
		for (int32 n = 0; n < EditedSplineComponent->GetNumberOfSplinePoints(); ++n)
			EditedSplineComponent->SetSplinePointType(n, ESplinePointType::Linear);
	}

	return true;
}

bool
FHoudiniSplineTranslator::UpdateOutputHoudiniSplineComponent(const TArray<FVector>& CurvePoints, UHoudiniSplineComponent* EditedHoudiniSplineComponent)
{
	if (!EditedHoudiniSplineComponent || EditedHoudiniSplineComponent->IsPendingKill())
		return false;

	if (CurvePoints.Num() < 2)
		return false;

	int MinCount = FMath::Min(CurvePoints.Num(), EditedHoudiniSplineComponent->CurvePoints.Num());
	
	int Idx = 0;
	// modify existing points
	for (Idx; Idx < MinCount; ++Idx)
	{
		FTransform CurTrans = EditedHoudiniSplineComponent->CurvePoints[Idx];
		if (CurTrans.GetLocation() == CurvePoints[Idx])
			continue;

		CurTrans.SetLocation(CurvePoints[Idx]);
	}

	// remove extra points
	if (Idx < EditedHoudiniSplineComponent->CurvePoints.Num()-1) 
	{
		for (int32 n = EditedHoudiniSplineComponent->CurvePoints.Num() - 1; n >= Idx; --n) 
		{
			EditedHoudiniSplineComponent->RemovePointAtIndex(n);
		}
	}


	// append extra points
	for (Idx; Idx < CurvePoints.Num(); ++Idx)
	{
		FTransform NewPoint = FTransform::Identity;
		NewPoint.SetLocation(CurvePoints[Idx]);
		EditedHoudiniSplineComponent->CurvePoints.Add(NewPoint);
	}

	return true;
}


bool 
FHoudiniSplineTranslator::CreateOutputSplinesFromHoudiniGeoPartObject(
	const FHoudiniGeoPartObject& InHGPO, 
	UHoudiniAssetComponent* InOuter,
	TMap<FHoudiniOutputObjectIdentifier, FHoudiniOutputObject>& InSplines, 
	TMap<FHoudiniOutputObjectIdentifier, FHoudiniOutputObject>& OutSplines,
	const bool& InForceRebuild) 
{
	// If we're not forcing the rebuild
	// No need to recreate something that hasn't changed
	if (!InForceRebuild && (!InHGPO.bHasGeoChanged || !InHGPO.bHasPartChanged))
	{
		// Simply reuse the existing meshes
		OutSplines = InSplines;
		return true;
	}

	if (!InOuter || InOuter->IsPendingKill())
		return false;

	int32 CurveNodeId = InHGPO.GeoId;
	int32 CurvePartId = InHGPO.PartId;
	if (CurveNodeId < 0 || CurvePartId < 0)
		return false;	

	// Extract all curve points from this HGPO
	TArray<float> RefinedCurvePositions;
	HAPI_AttributeInfo AttributeRefinedCurvePositions;
	FHoudiniApi::AttributeInfo_Init(&AttributeRefinedCurvePositions);
	FHoudiniEngineUtils::HapiGetAttributeDataAsFloat(
		CurveNodeId, CurvePartId, HAPI_UNREAL_ATTRIB_POSITION, AttributeRefinedCurvePositions, RefinedCurvePositions);

	TArray<float> RefinedCurveRotations;
	HAPI_AttributeInfo AttributeRefinedCurveRotations;
	FHoudiniApi::AttributeInfo_Init(&AttributeRefinedCurveRotations);
	FHoudiniEngineUtils::HapiGetAttributeDataAsFloat(
		CurveNodeId, CurvePartId, HAPI_UNREAL_ATTRIB_ROTATION, AttributeRefinedCurveRotations, RefinedCurveRotations);

	TArray<float> RefinedCurveScales;
	HAPI_AttributeInfo AttributeRefinedCurveScales;
	FHoudiniApi::AttributeInfo_Init(&AttributeRefinedCurveScales);
	FHoudiniEngineUtils::HapiGetAttributeDataAsFloat(
		CurveNodeId, CurvePartId, HAPI_UNREAL_ATTRIB_SCALE, AttributeRefinedCurveScales, RefinedCurveScales);

	HAPI_CurveInfo CurveInfo;
	FHoudiniApi::CurveInfo_Init(&CurveInfo);
	FHoudiniApi::GetCurveInfo(FHoudiniEngine::Get().GetSession(), CurveNodeId, CurvePartId, &CurveInfo);

	int32 NumOfCurves = CurveInfo.curveCount;
	TArray<int32> CurvePointsCounts;
	CurvePointsCounts.SetNumZeroed(NumOfCurves);
	FHoudiniApi::GetCurveCounts(FHoudiniEngine::Get().GetSession(), CurveNodeId, CurvePartId, CurvePointsCounts.GetData(), 0, NumOfCurves);

	TArray<TArray<FVector>> CurvesDisplayPoints;
	FHoudiniSplineTranslator::ConvertToVectorData(RefinedCurvePositions, CurvesDisplayPoints, CurvePointsCounts);

	TArray<TArray<FVector>> CurvesRotations;
	FHoudiniSplineTranslator::ConvertToVectorData(RefinedCurveRotations, CurvesRotations, CurvePointsCounts);

	TArray<TArray<FVector>> CurvesScales;
	FHoudiniSplineTranslator::ConvertToVectorData(RefinedCurveScales, CurvesScales, CurvePointsCounts);

	// Extract all curve points from this HGPO
	FString GeoName = InHGPO.PartName;
	int32 CurveIdx = 1;

	// Iterate through all curves found in this HGPO
	for (int32 n = 0; n < CurvesDisplayPoints.Num(); ++n) 
	{
		FString CurveName = FString::Printf(TEXT("%s curve %d"), *GeoName, CurveIdx);
		CurveIdx += 1;

		if (CurvePointsCounts[n] < 2)
		{
			// Invalid vertex count, skip this curve.
			HOUDINI_LOG_WARNING(
				TEXT("Creating Unreal Spline: Object [%d %s], Geo [%d], Part [%d %s], Curve# [%d] invalid vertex count.")
				TEXT("- skipping."),
				InHGPO.ObjectId, *InHGPO.ObjectName, InHGPO.GeoId, InHGPO.PartId, *InHGPO.PartName, CurveIdx);
			continue;
		}

		FHoudiniOutputObjectIdentifier CurveIdentifier(InHGPO.ObjectId, InHGPO.GeoId, InHGPO.PartId, CurveName);		
		FHoudiniOutputObject* FoundOutputObject = InSplines.Find(CurveIdentifier);

		bool bNeedToRebuildSpline = false;
		if (!FoundOutputObject)
			bNeedToRebuildSpline = true;

		USceneComponent* FoundComponent = Cast<USceneComponent>(FoundOutputObject ? FoundOutputObject->OutputComponent : nullptr);
		if (FoundComponent && !FoundComponent->IsPendingKill())
		{
			if (FoundComponent->IsA<USplineComponent>() && FoundOutputObject->CurveOutputProperty.CurveOutputType != EHoudiniCurveOutputType::UnrealSpline)
				bNeedToRebuildSpline = true;

			if (FoundComponent->IsA<UHoudiniSplineComponent>() && FoundOutputObject->CurveOutputProperty.CurveOutputType != EHoudiniCurveOutputType::HoudiniSpline)
				bNeedToRebuildSpline = true;

			if (InHGPO.bHasGeoChanged || InHGPO.PartInfo.bHasChanged || InForceRebuild)
				bNeedToRebuildSpline = true;
		}
		else
		{
			bNeedToRebuildSpline = true;
		}

		// The curve has not changed, no need to go through the rest
		if (!bNeedToRebuildSpline)
		{
			OutSplines.Add(CurveIdentifier, *FoundOutputObject);
			continue;
		}

		bool bReusedPreviousOutput = false;
		if (!FoundOutputObject) 
		{
			HOUDINI_LOG_WARNING(
				TEXT("Creating Unreal Spline (default): Object [%d %s], Geo [%d], Part [%d %s], Curve# [%d], number of points [%d]."),
				InHGPO.ObjectId, *InHGPO.ObjectName, InHGPO.GeoId, InHGPO.PartId, *InHGPO.PartName, CurveIdx, CurvePointsCounts[n]);
			
			// If not found (at initialize), create an Unreal spline by defaults
			USplineComponent* CreatedSplineComponent = FHoudiniSplineTranslator::CreateOutputUnrealSplineComponent(CurvesDisplayPoints[n], CurvesRotations[n], CurvesScales[n], InOuter);
			if (!CreatedSplineComponent)
				continue;

			// Create a new output object
			FHoudiniOutputObject NewOutputObject;
			NewOutputObject.OutputComponent = CreatedSplineComponent;

			NewOutputObject.CurveOutputProperty.CurveOutputType = EHoudiniCurveOutputType::UnrealSpline;
			NewOutputObject.CurveOutputProperty.NumPoints = CurvePointsCounts[n];

			// TODO: Need a way to access info of the output curve
			NewOutputObject.CurveOutputProperty.CurveMethod = EHoudiniCurveMethod::Breakpoints;
			NewOutputObject.CurveOutputProperty.CurveType = EHoudiniCurveType::Linear;
			NewOutputObject.CurveOutputProperty.bClosed = false;
			// Fill in the rest of output curve properties

			OutSplines.Add(CurveIdentifier, NewOutputObject);
		}
		else 
		{
			// 
			if (FoundOutputObject->CurveOutputProperty.CurveOutputType == EHoudiniCurveOutputType::UnrealSpline)
			{
				// See if we can simply update the previous Spline Component
				bool bCanUpdateUnrealSpline = (FoundOutputObject->OutputComponent &&  FoundOutputObject->OutputComponent->IsA<USplineComponent>());
				if (bCanUpdateUnrealSpline)
				{
					// Update the existing unreal spline component
					bReusedPreviousOutput = true;
					HOUDINI_LOG_WARNING(
						TEXT("Updating Unreal Spline: Object [%d %s], Geo [%d], Part [%d %s], Curve# [%d], number of points [%d]."),
						InHGPO.ObjectId, *InHGPO.ObjectName, InHGPO.GeoId, InHGPO.PartId, *InHGPO.PartName, CurveIdx, CurvePointsCounts[n]);

					USplineComponent* FoundUnrealSpline = Cast<USplineComponent>(FoundOutputObject->OutputComponent);
					if (!FHoudiniSplineTranslator::UpdateOutputUnrealSplineComponent(CurvesDisplayPoints[n], FoundUnrealSpline, FoundOutputObject->CurveOutputProperty.CurveType))
						continue;
					
					OutSplines.Add(CurveIdentifier, *FoundOutputObject);
				}
				else
				{
					// Create a new Unreal spline component
					bReusedPreviousOutput = false;
					FoundOutputObject->CurveOutputProperty.CurveOutputType = EHoudiniCurveOutputType::UnrealSpline;					
					HOUDINI_LOG_WARNING(
						TEXT("Creating Unreal Spline: Object [%d %s], Geo [%d], Part [%d %s], Curve# [%d], number of points [%d]."),
						InHGPO.ObjectId, *InHGPO.ObjectName, InHGPO.GeoId, InHGPO.PartId, *InHGPO.PartName, CurveIdx, CurvePointsCounts[n]);

					USplineComponent* NewUnrealSpline = FHoudiniSplineTranslator::CreateOutputUnrealSplineComponent(CurvesDisplayPoints[n], CurvesRotations[n], CurvesScales[n], InOuter);
					if (!NewUnrealSpline)
						continue;

					FoundOutputObject->OutputComponent = NewUnrealSpline;

					OutSplines.Add(CurveIdentifier, *FoundOutputObject);
				}
			}
			else
			{
				// We want to output a Houdini Spline Component
				// See if we can simply update the previous Houdini Spline Component
				bool bCanUpdateHoudiniSpline = (FoundOutputObject->OutputComponent &&  FoundOutputObject->OutputComponent->IsA<UHoudiniSplineComponent>());
				if (bCanUpdateHoudiniSpline)
				{
					// Update the existing houdini spline component
					bReusedPreviousOutput = true;
					HOUDINI_LOG_WARNING(
						TEXT("Changing Houdini Spline: Object [%d %s], Geo [%d], Part [%d %s], Curve# [%d], number of points [%d]."),
						InHGPO.ObjectId, *InHGPO.ObjectName, InHGPO.GeoId, InHGPO.PartId, *InHGPO.PartName, CurveIdx, CurvePointsCounts[n]);

					UHoudiniSplineComponent* FoundHoudiniSpline = Cast<UHoudiniSplineComponent>(FoundOutputObject->OutputComponent);
					if (!FHoudiniSplineTranslator::UpdateOutputHoudiniSplineComponent(CurvesDisplayPoints[n], FoundHoudiniSpline))
						continue;

					OutSplines.Add(CurveIdentifier, *FoundOutputObject);
				}
				else
				{
					// Create a new Houdini spline component
					bReusedPreviousOutput = false;
					FoundOutputObject->CurveOutputProperty.CurveOutputType = EHoudiniCurveOutputType::HoudiniSpline;
					HOUDINI_LOG_WARNING(
						TEXT("Creating Unreal Spline: Object [%d %s], Geo [%d], Part [%d %s], Curve# [%d], number of points [%d]."),
						InHGPO.ObjectId, *InHGPO.ObjectName, InHGPO.GeoId, InHGPO.PartId, *InHGPO.PartName, CurveIdx, CurvePointsCounts[n]);

					UHoudiniSplineComponent* NewHoudiniSpline = CreateOutputHoudiniSplineComponent(CurvesDisplayPoints[n], CurvesRotations[n], CurvesScales[n], InOuter);
					if (!NewHoudiniSpline)
						continue;

					FoundOutputObject->OutputComponent = NewHoudiniSpline;

					OutSplines.Add(CurveIdentifier, *FoundOutputObject);
				}
			}
		}

		if (bReusedPreviousOutput)
		{
			// Remove the reused output unreal spline from the old map to avoid its deletion
			InSplines.Remove(CurveIdentifier);
		}

		HOUDINI_LOG_WARNING(
			TEXT("Finished Generating Unreal Spline: Object [%d %s], Geo [%d], Part [%d %s], Curve# [%d], number of points [%d]."),
			InHGPO.ObjectId, *InHGPO.ObjectName, InHGPO.GeoId, InHGPO.PartId, *InHGPO.PartName, CurveIdx, CurvePointsCounts[n]);
	}

	return true;
}


bool 
FHoudiniSplineTranslator::CreateAllSplinesFromHoudiniOutput(UHoudiniOutput* InOutput, UObject* InOuterComponent)
{
	if (!InOutput || InOutput->IsPendingKill())
		return false;

	if (!InOuterComponent || InOuterComponent->IsPendingKill())
		return false;

	// ONLY DO THIS ON CURVES!!!!
	if (InOutput->GetType() != EHoudiniOutputType::Curve)
		return false;

	UHoudiniAssetComponent* OuterHAC = Cast<UHoudiniAssetComponent>(InOuterComponent);

	if (!OuterHAC || OuterHAC->IsPendingKill())
		return false;

	TMap<FHoudiniOutputObjectIdentifier, FHoudiniOutputObject> NewOutputObjects;
	TMap<FHoudiniOutputObjectIdentifier, FHoudiniOutputObject>& OldOutputObjects = InOutput->GetOutputObjects();

	// Iterate on all the output's HGPO
	for (const FHoudiniGeoPartObject & CurHGPO : InOutput->GetHoudiniGeoPartObjects())
	{
		// not a curve, skip
		if (CurHGPO.Type != EHoudiniPartType::Curve)
			continue;

		CreateOutputSplinesFromHoudiniGeoPartObject(CurHGPO, OuterHAC, OldOutputObjects, NewOutputObjects, false);
	}

	// TODO: FIX ME!!! This literally nukes all the output objects, even if they are not curves!

	// The old map now only contains unused/stale output curves destroy them
	for (auto& OldPair : OldOutputObjects)
	{
		USceneComponent* OldSplineSceneComponent = Cast<USceneComponent>(OldPair.Value.OutputComponent);
		if (!OldSplineSceneComponent || OldSplineSceneComponent->IsPendingKill())
			continue;

		OldSplineSceneComponent->DetachFromComponent(FDetachmentTransformRules::KeepRelativeTransform);
		OldSplineSceneComponent->UnregisterComponent();
		OldSplineSceneComponent->DestroyComponent();
	}
	OldOutputObjects.Empty();

	InOutput->SetOutputObjects(NewOutputObjects);

	FHoudiniEngineUtils::UpdateEditorProperties(InOutput, true);

	return true;
}

void
FHoudiniSplineTranslator::ReselectSelectedActors()
{
	// TODO: Duplicate with FHoudiniEngineUtils::UpdateEditorProperties ??
	USelection* Selection = GEditor->GetSelectedActors();
	TArray<AActor*> SelectedActors;
	SelectedActors.SetNumUninitialized(GEditor->GetSelectedActorCount());
	Selection->GetSelectedObjects(SelectedActors);

	GEditor->SelectNone(false, false, false);

	for (AActor* NextSelected : SelectedActors)
	{
		GEditor->SelectActor(NextSelected, true, true, true, true);
	}
}