//------------------------------------------------------------------
//
//   MODULE  : LTBMODELFX.CPP
//
//   PURPOSE : Implements class CLTBModelFX
//
//   CREATED : On 12/3/98 At 6:34:44 PM
//
//------------------------------------------------------------------

//
// Includes....
//

	#include "stdafx.h"
	#include "LTBModelFX.h"
	#include "ClientFX.h"

//
// Defines...
//

	#define FACE_CAMERAFACING	0
	#define	FACE_PARENTALIGN	2

// ----------------------------------------------------------------------- //
//
//  ROUTINE:	CLTBModelProps::CLTBModelProps
//
//  PURPOSE:	Constructor
//
// ----------------------------------------------------------------------- //
CLTBModelProps::CLTBModelProps() : 
	m_vNorm					( 0.0f, 0.0f, 1.0f ),
	m_nFacing				( 0 ),
	m_bShadow				( LTFALSE ),
	m_bOverrideAniLength	( LTFALSE ),
	m_fAniLength			( 0.0f ),
	m_bSyncToModelAnim		( LTFALSE ),
	m_bSyncToKey			( LTFALSE ),
	m_bRotate				( LTFALSE )
{
	m_szModelName[0] = '\0';
	m_szAnimName[0]  = '\0';
	
	m_szSkinName[0][0] = '\0';
	m_szSkinName[1][0] = '\0';
	m_szSkinName[2][0] = '\0';
	m_szSkinName[3][0] = '\0';
	m_szSkinName[4][0] = '\0';
	
	m_szRenderStyle[0][0] = '\0';
	m_szRenderStyle[1][0] = '\0';
	m_szRenderStyle[2][0] = '\0';
	m_szRenderStyle[3][0] = '\0';
}

// ----------------------------------------------------------------------- //
//
//  ROUTINE:	CCamJitterProps::ReadProps
//
//  PURPOSE:	Read in the proporty values that were set in FxED
//
// ----------------------------------------------------------------------- //

bool CLTBModelProps::ParseProperties(FX_PROP* pProps, uint32 nNumProps)
{
	if(!CBaseFXProps::ParseProperties(pProps, nNumProps))
		return false;

	//
	// Loop through the props to initialize data
	//
	for(uint32 nCurrProp = 0; nCurrProp < nNumProps; nCurrProp++)
	{
		FX_PROP& fxProp = pProps[nCurrProp];

		if( !_stricmp( fxProp.m_sName, "Model" ))
		{
			fxProp.GetPath( m_szModelName );
		}
		else if( !_stricmp( fxProp.m_sName, "Skin0" ))
		{
			fxProp.GetPath( m_szSkinName[0] );
		}
		else if( !_stricmp( fxProp.m_sName, "Skin1" ))
		{
			fxProp.GetPath( m_szSkinName[1] );
		}
		else if( !_stricmp( fxProp.m_sName, "Skin2" ))
		{
			fxProp.GetPath( m_szSkinName[2] );
		}
		else if( !_stricmp( fxProp.m_sName, "Skin3" ))
		{
			fxProp.GetPath( m_szSkinName[3] );
		}
		else if( !_stricmp( fxProp.m_sName, "Skin4" ))
		{
			fxProp.GetPath( m_szSkinName[4] );
		}
		else if( !_stricmp( fxProp.m_sName, "Facing" ))
		{
			m_nFacing = fxProp.GetComboVal();
		}
		else if( !_stricmp( fxProp.m_sName, "Normal" ))
		{
			m_vNorm = fxProp.GetVector();

			// Dont norm a zero length vector!

			if( m_vNorm.LengthSquared() > MATH_EPSILON )
			{
				m_vNorm.Normalize();
			}
			else
			{
				// Just kludge the forward down the Z

				m_vNorm.Init( 0.0f, 0.0f, 1.0f );
			}

		}
		else if( !_stricmp( fxProp.m_sName, "Shadow" ))
		{
			m_bShadow = (LTBOOL)fxProp.GetComboVal();
		}
		else if( !_stricmp( fxProp.m_sName, "OverrideAniLength" ))
		{
			m_bOverrideAniLength = (LTBOOL)fxProp.GetComboVal();
		}
		else if( !_stricmp( fxProp.m_sName, "AniName" ))
		{
			fxProp.GetStringVal(m_szAnimName);
		}
		else if( !_stricmp( fxProp.m_sName, "AniLength" ))
		{
			m_fAniLength = fxProp.GetFloatVal();
		}
		else if( !_stricmp( fxProp.m_sName, "RenderStyle0" ))
		{
			fxProp.GetPath( m_szRenderStyle[0] );
		}
		else if( !_stricmp( fxProp.m_sName, "RenderStyle1" ))
		{
			fxProp.GetPath( m_szRenderStyle[1] );
		}
		else if( !_stricmp( fxProp.m_sName, "RenderStyle2" ))
		{
			fxProp.GetPath( m_szRenderStyle[2] );
		}
		else if( !_stricmp( fxProp.m_sName, "RenderStyle3" ))
		{
			fxProp.GetPath( m_szRenderStyle[3] );
		}
		else if( !_stricmp( fxProp.m_sName, "SyncToModelAnim" ) )
		{
			m_bSyncToModelAnim = (LTBOOL)fxProp.GetComboVal();
			m_bSyncToKey = LTFALSE;
		}
		else if( !_stricmp( fxProp.m_sName, "SyncToKey" ) )
		{
			m_bSyncToKey = (LTBOOL)fxProp.GetComboVal();
			m_bSyncToModelAnim = LTFALSE;
		}
	}

	m_bRotate = ( (m_vRotAdd.x != 0.0f) || (m_vRotAdd.y != 0.0f) || (m_vRotAdd.z != 0.0f) ) ? LTTRUE : LTFALSE ;

	return true;
}


//------------------------------------------------------------------
//
//   FUNCTION : CLTBModelFX()
//
//   PURPOSE  : Standard constuctor
//
//------------------------------------------------------------------

CLTBModelFX::CLTBModelFX()
:	CBaseFX					( CBaseFX::eLTBModelFX ),
	m_fAniRate				( 0.0f )
{
}

//------------------------------------------------------------------
//
//   FUNCTION : ~CLTBModelFX
//
//   PURPOSE  : Standard destructor
//
//------------------------------------------------------------------

CLTBModelFX::~CLTBModelFX()
{
	Term();
}

//------------------------------------------------------------------
//
//   FUNCTION : Init()
//
//   PURPOSE  : Initialises class CLTBModelFX
//
//------------------------------------------------------------------

// ⚠️ EXPERIMENT HOOK, default OFF: LT_FX_ROTXYZ="x,y,z" (DEGREES) adds a
// rotation to every LTBModel effect, about the model's own right/up/forward
// (x = pitch, y = yaw, z = roll). Exists to TEST an "it needs turning N
// degrees" report without guessing in code — §56's C01S01 waterfall.
// If a value makes it look right that is a measurement; if none do, the
// hypothesis is dead and nothing shipped. NEVER enable this by default.
//
// ⚠️⚠️ MUST BE APPLIED IN **Update()** TOO, NOT ONLY Init(). These models are
// Facing=ParentAlign WITH a parent, so Update() calls SetObjectRotation every
// frame from the parent's rotation and throws away whatever Init() set. The
// first version of this hook only touched Init and therefore did NOTHING,
// which reads exactly like "the rotation had no effect".
static bool fx_DebugRotate(LTRotation &r)
{
	static int s_nHave = -1;
	static float s_fX = 0.0f, s_fY = 0.0f, s_fZ = 0.0f;
	if (s_nHave < 0)
	{
		const char *p = getenv("LT_FX_ROTXYZ");
		s_nHave = (p && sscanf(p, "%f,%f,%f", &s_fX, &s_fY, &s_fZ) == 3) ? 1 : 0;
	}
	if (s_nHave != 1)
		return false;

	LTVector vR = r.Right(), vU = r.Up(), vF = r.Forward();
	if (s_fX != 0.0f) r.Rotate(vR, MATH_DEGREES_TO_RADIANS(s_fX));
	if (s_fY != 0.0f) r.Rotate(vU, MATH_DEGREES_TO_RADIANS(s_fY));
	if (s_fZ != 0.0f) r.Rotate(vF, MATH_DEGREES_TO_RADIANS(s_fZ));
	return true;
}

bool CLTBModelFX::Init(ILTClient *pClientDE, FX_BASEDATA *pBaseData, const CBaseFXProps *pProps)
{
	// Perform base class initialisation

	if (!CBaseFX::Init(pClientDE, pBaseData, pProps)) 
		return false;

	// Use the "target" Normal instead, if one was specified...

	LTVector vNorm = GetProps()->m_vNorm;

	if( pBaseData->m_vTargetNorm.LengthSquared() > MATH_EPSILON )
	{
		vNorm = pBaseData->m_vTargetNorm;
		vNorm.Normalize();
	}

	// Develop the Right and Up vectors based off the Forward...

	LTVector vR, vU;
	if( (1.0f == vNorm.y) || (-1.0f == vNorm.y) )
	{
		vR = LTVector( 1.0f, 0.0f, 0.0f ).Cross( vNorm );
	}
	else
	{
		vR = LTVector( 0.0f, 1.0f, 0.0f ).Cross( vNorm );
	}

	vU = vNorm.Cross( vR );
	m_rNormalRot = LTRotation( vNorm, vU );


	ObjectCreateStruct ocs;

	// Combine the direction we would like to face with our parents rotation...

	if( m_hParent )
	{
		m_pLTClient->GetObjectRotation( m_hParent, &ocs.m_Rotation );
	}
	else
	{
		ocs.m_Rotation = m_rCreateRot;
	}

	// §56: the C01S01 waterfall (JA_Waterfall = WATERFALL1.LTB + WATERFALL2.LTB)
	// renders but is turned 90 degrees. The orientation maths downstream is
	// verified correct (quat<->matrix, Right/Up/Forward, LTRotation(fwd,up)
	// round-trip), so print the INPUTS: the authored normal, whether a target
	// normal overrode it, the create/parent rotation, and the result.
	if (getenv("LT_TRACE_FX"))
	{
		LTVector cR = ocs.m_Rotation.Right(), cU = ocs.m_Rotation.Up(), cF = ocs.m_Rotation.Forward();
		LTRotation rFinal = ocs.m_Rotation * m_rNormalRot;
		LTVector fR = rFinal.Right(), fU = rFinal.Up(), fF = rFinal.Forward();
		fprintf(stderr,
		        "[fx]   LTBModel '%s'\n"
		        "[fx]     norm=(%.2f %.2f %.2f) targetNorm=(%.2f %.2f %.2f) parent=%d\n"
		        "[fx]     createRot fwd=(%.2f %.2f %.2f) up=(%.2f %.2f %.2f) right=(%.2f %.2f %.2f)\n"
		        "[fx]     FINAL     fwd=(%.2f %.2f %.2f) up=(%.2f %.2f %.2f) right=(%.2f %.2f %.2f)\n",
		        GetProps()->m_szModelName[0] ? GetProps()->m_szModelName : "<none>",
		        vNorm.x, vNorm.y, vNorm.z,
		        pBaseData->m_vTargetNorm.x, pBaseData->m_vTargetNorm.y, pBaseData->m_vTargetNorm.z,
		        m_hParent ? 1 : 0,
		        cF.x, cF.y, cF.z, cU.x, cU.y, cU.z, cR.x, cR.y, cR.z,
		        fF.x, fF.y, fF.z, fU.x, fU.y, fU.z, fR.x, fR.y, fR.z);
	}

	ocs.m_Rotation = ocs.m_Rotation * m_rNormalRot;

	fx_DebugRotate(ocs.m_Rotation);

	ocs.m_ObjectType		= OT_MODEL;
	ocs.m_Flags				|= pBaseData->m_dwObjectFlags |	(GetProps()->m_bShadow ? FLAG_SHADOW : 0 );
	ocs.m_Flags2			|= pBaseData->m_dwObjectFlags2;
	
	// Calculate the position with the offset in 'local' coordinate space...

	LTMatrix mMat;
	ocs.m_Rotation.ConvertToMatrix( mMat );

	m_vPos = ocs.m_Pos = m_vCreatePos + (mMat * GetProps()->m_vOffset);

	
	SAFE_STRCPY( ocs.m_Filename, GetProps()->m_szModelName );
	
	SAFE_STRCPY( ocs.m_SkinNames[0], GetProps()->m_szSkinName[0] );
	SAFE_STRCPY( ocs.m_SkinNames[1], GetProps()->m_szSkinName[1] );
	SAFE_STRCPY( ocs.m_SkinNames[2], GetProps()->m_szSkinName[2] );
	SAFE_STRCPY( ocs.m_SkinNames[3], GetProps()->m_szSkinName[3] );
	SAFE_STRCPY( ocs.m_SkinNames[4], GetProps()->m_szSkinName[4] );
	
	SAFE_STRCPY( ocs.m_RenderStyleNames[0], GetProps()->m_szRenderStyle[0] );
	SAFE_STRCPY( ocs.m_RenderStyleNames[1], GetProps()->m_szRenderStyle[1] );
	SAFE_STRCPY( ocs.m_RenderStyleNames[2], GetProps()->m_szRenderStyle[2] );
	SAFE_STRCPY( ocs.m_RenderStyleNames[3], GetProps()->m_szRenderStyle[3] );

	m_hObject = m_pLTClient->CreateObject(&ocs);
	if( !m_hObject ) 
		return LTFALSE;

	ILTModel		*pLTModel = m_pLTClient->GetModelLT();
	ANIMTRACKERID	nTracker;
	
	pLTModel->GetMainTracker( m_hObject, nTracker );

	//setup the animation if the user specified one
	if( strlen(GetProps()->m_szAnimName) > 0)
	{
		//ok, we need to set this
		HMODELANIM hAnim = m_pLTClient->GetAnimIndex(m_hObject, GetProps()->m_szAnimName);

		if(hAnim != INVALID_MODEL_ANIM)
		{
			//ok, lets set this animation
			pLTModel->SetCurAnim(m_hObject, nTracker, hAnim);
			pLTModel->ResetAnim(m_hObject, nTracker);
		}
	}
	//disable looping on this animation (so we can actually stop!)
	pLTModel->SetLooping(m_hObject, nTracker, false);

	// Setup the initial data needed to override the models animation length...
	if( GetProps()->m_bOverrideAniLength )
	{
		uint32 nAnimLength;

		pLTModel->GetCurAnimLength( m_hObject, nTracker, nAnimLength );
		pLTModel->SetCurAnimTime( m_hObject, nTracker, 0 );

		float fAniLength = (GetProps()->m_fAniLength < MATH_EPSILON) ? GetProps()->m_tmLifespan : GetProps()->m_fAniLength;
		
		if( fAniLength >= MATH_EPSILON || fAniLength <= -MATH_EPSILON )
			m_fAniRate = (nAnimLength * 0.001f) / fAniLength;

		pLTModel->SetAnimRate( m_hObject, nTracker, m_fAniRate );
	}

	// Success !!

	return LTTRUE;
}

//------------------------------------------------------------------
//
//   FUNCTION : Term()
//
//   PURPOSE  : Terminates class CLTBModelFX
//
//------------------------------------------------------------------

bool CLTBModelFX::IsFinishedShuttingDown()
{
	//if we are syncing to the model key, we are always done
	if(GetProps()->m_bSyncToKey)
	{
		return true;
	}

	//otherwise just ask the animation if it has completed
	ANIMTRACKERID	nTracker;
	m_pLTClient->GetModelLT()->GetMainTracker( m_hObject, nTracker );

	uint32 dwState = 0;
	m_pLTClient->GetModelLT()->GetPlaybackState(m_hObject,nTracker,dwState);

	return (bool)(!!(dwState & MS_PLAYDONE));
}

//------------------------------------------------------------------
//
//   FUNCTION : Term()
//
//   PURPOSE  : Terminates class CLTBModelFX
//
//------------------------------------------------------------------

void CLTBModelFX::Term()
{
	if (m_hObject) m_pLTClient->RemoveObject(m_hObject);
	m_hObject = NULL;
}

//------------------------------------------------------------------
//
//   FUNCTION : Update()
//
//   PURPOSE  : Updates class CLTBModelFX
//
//------------------------------------------------------------------

bool CLTBModelFX::Update(float tmFrameTime)
{

	//Ok, what we are going to do is see if we are supposed to be sync'd to the
	//animation. If so, we are going to flat out ignore tmCur, and instead generate
	//our own. This way we can match the model exactly.
	if(GetProps()->m_bSyncToModelAnim)
	{
		//we need to find out where in the animation the model currently is
		ILTModel		*pLTModel = m_pLTClient->GetModelLT();
		ANIMTRACKERID	nTracker;
		
		if(pLTModel->GetMainTracker( m_hObject, nTracker ) == LT_OK)
		{
			//we have the main tracker, see where in its timeline it is
			uint32 nCurrTime;
			uint32 nAnimTime;

			pLTModel->GetCurAnimTime(m_hObject, nTracker, nCurrTime);
			pLTModel->GetCurAnimLength(m_hObject, nTracker, nAnimTime);

			if(nAnimTime)
			{
				//handle wrapping
				nCurrTime %= nAnimTime;

				//ok, now convert cur time to a valid time
				m_tmElapsed = (nCurrTime * GetProps()->m_tmLifespan) / (float)nAnimTime;
			}
			else
			{
				//zero length animation?
				m_tmElapsed = 0.0f;
			}
		}
	}
	else if(GetProps()->m_bSyncToKey)
	{
		//we need to find out where in the key we currently are
		ILTModel		*pLTModel = m_pLTClient->GetModelLT();
		ANIMTRACKERID	nTracker;
		
		if(pLTModel->GetMainTracker( m_hObject, nTracker ) == LT_OK)
		{
			//we have the main tracker, see where in its timeline it is
			uint32 nAnimLength;
			m_pLTClient->GetModelLT()->ResetAnim( m_hObject, nTracker );
			pLTModel->GetCurAnimLength(m_hObject, nTracker, nAnimLength);

			if(nAnimLength > 0)
				nAnimLength--;

			float tmWrappedTime = fmodf(m_tmElapsed / GetProps()->m_tmLifespan, 1.0f);
			uint32 nAnimTime = (uint32)(tmWrappedTime * nAnimLength);
			pLTModel->SetCurAnimTime(m_hObject, nTracker, nAnimTime);
		}
	}
	
	// Base class update first	
	if (!CBaseFX::Update(tmFrameTime)) 
		return false;


	//see if we should reset our model animation
	if(!GetProps()->m_bSyncToKey && IsFinishedShuttingDown())
	{
		//Reset the animation
		ANIMTRACKERID	nTracker;
		
		m_pLTClient->GetModelLT()->GetMainTracker( m_hObject, nTracker );
		m_pLTClient->GetModelLT()->ResetAnim( m_hObject, nTracker );

		if(GetProps()->m_bOverrideAniLength)
			m_pLTClient->GetModelLT()->SetAnimRate( m_hObject, nTracker, m_fAniRate);
	}

	// Align if neccessary, to the rotation of our parent

	if ((m_hParent) && (GetProps()->m_nFacing == FACE_PARENTALIGN))
	{
		LTRotation rParentRot;
		m_pLTClient->GetObjectRotation(m_hParent, &rParentRot);
		rParentRot = (GetProps()->m_bRotate ? rParentRot : rParentRot * m_rNormalRot);

		// ⚠️⚠️ EMPIRICAL CORRECTION, ROOT CAUSE UNKNOWN — see §58.
		// A parent-aligned LTBModel comes out 90 degrees of YAW off: with it,
		// C01S01's waterfall sheet hangs out sideways from the cliff instead of
		// lying along it (user-verified, and -90 yaw makes the whole scene
		// read correctly).
		//
		// This is NOT a guess at the maths — the entire rotation pipeline was
		// verified sound first and NONE of it is the cause:
		//   * quat_ConvertToMatrix vs LTRotation::Right/Up/Forward — the
		//     accessors ARE its columns 0/1/2 (checked term by term);
		//   * LTRotation(vForward,vUp) — standalone round-trip harness,
		//     Forward() reproduces the input normal exactly on all six axes;
		//   * quat -> matrix -> quat round-trips; SetBasisVectors writes
		//     COLUMNS and quat_ConvertFromMatrix reads them consistently (so
		//     there is no transpose/inverse, which would have looked exactly
		//     like a sign-flipped yaw);
		//   * CCompress::UncompressRotation's fixup is self-consistent with
		//     the engine's REVERSED LTVector::Cross (a.Cross(b) == b x a,
		//     original Monolith code, not a port artifact);
		//   * m_rNormalRot is IDENTITY here (Normal=(0,0,1)), so the model
		//     inherits the parent's rotation verbatim.
		//
		// ⚠️ Scoped deliberately to the PARENT-ALIGNED branch. LTBModel effects
		// with no parent (e.g. INTERFACE\MENU\MODELS\LOADING.LTB) go through
		// Init() instead and must NOT be rotated — a global correction breaks
		// them.
		// ⚠️ UNVALIDATED OUTSIDE C01S01. If a parent-aligned FX model looks
		// turned in another level, this is the first thing to switch off:
		// LT_FX_NOPARENTYAW=1.
		{
			static int s_nOff = -1;
			if (s_nOff < 0) s_nOff = getenv("LT_FX_NOPARENTYAW") ? 1 : 0;
			if (!s_nOff)
			{
				LTVector vUp = rParentRot.Up();
				rParentRot.Rotate(vUp, MATH_DEGREES_TO_RADIANS(-90.0f));
			}
		}

		fx_DebugRotate(rParentRot);          // §56 experiment hook — stacks on top
		m_pLTClient->SetObjectRotation(m_hObject, &rParentRot);
	}
	

	// If we want to add a rotation, make sure we are facing the correct way...
	
	if( GetProps()->m_bRotate )
	{
		LTFLOAT		tmFrame = tmFrameTime;
		LTVector	vR( m_rRot.Right() );
		LTVector	vU( m_rRot.Up() );
		LTVector	vF( m_rRot.Forward() );

		LTRotation	rRotation;

		if( m_hCamera && (GetProps()->m_nFacing == FACE_CAMERAFACING))
		{
			m_pLTClient->GetObjectRotation( m_hCamera, &rRotation );
		}
		else
		{
			m_pLTClient->GetObjectRotation( m_hObject, &rRotation );
		}

		m_rRot.Rotate( vR, MATH_DEGREES_TO_RADIANS( GetProps()->m_vRotAdd.x * tmFrame ));
		m_rRot.Rotate( vU, MATH_DEGREES_TO_RADIANS( GetProps()->m_vRotAdd.y * tmFrame ));
		m_rRot.Rotate( vF, MATH_DEGREES_TO_RADIANS( GetProps()->m_vRotAdd.z * tmFrame ));
		
		rRotation = rRotation * m_rRot;
		
		m_pLTClient->SetObjectRotation( m_hObject, &(rRotation * m_rNormalRot));
	}
	else if( GetProps()->m_nFacing == FACE_CAMERAFACING )
	{
		LTRotation rCamRot;

		m_pLTClient->GetObjectRotation( m_hCamera, &rCamRot );
		m_pLTClient->SetObjectRotation( m_hObject, &(rCamRot * m_rNormalRot) );
	}

	// Success !!

	return true;
}


//------------------------------------------------------------------
//
//   FUNCTION : fxGetModelFXProps()
//
//   PURPOSE  : Returns a list of properties for this FX
//
//------------------------------------------------------------------

void fxGetLTBModelProps(CFastList<FX_PROP> *pList)
{
	FX_PROP fxProp;
	float fVec[3];
	fVec[0] = 0.0f;
	fVec[1] = 0.0f;
	fVec[2] = 1.0f;

	// Add the base props

	AddBaseProps(pList);

	// Add all the props to the list

	fxProp.Path( "Model", "ltb|..." );
	pList->AddTail(fxProp);

	fxProp.Path( "Skin0", "dtx|..." );
	pList->AddTail(fxProp);

	fxProp.Path( "Skin1", "dtx|..." );
	pList->AddTail( fxProp );
	
	fxProp.Path( "Skin2", "dtx|..." );
	pList->AddTail( fxProp );

	fxProp.Path( "Skin3", "dtx|..." );
	pList->AddTail( fxProp );

	fxProp.Path( "Skin4", "dtx|..." );
	pList->AddTail( fxProp );

	fxProp.Vector( "Normal", fVec );
	pList->AddTail( fxProp );

	fxProp.Combo( "Facing", "2,CameraFacing,AlongNormal,ParentAlign" );
	pList->AddTail( fxProp );

	fxProp.Combo( "Shadow", "0,No,Yes" );
	pList->AddTail( fxProp );

	fxProp.Combo( "OverrideAniLength", "0,No,Yes" );
	pList->AddTail( fxProp );

	fxProp.String( "AniName", "");
	pList->AddTail( fxProp );

	fxProp.Float( "AniLength", 0 );
	pList->AddTail( fxProp );

	fxProp.Path( "RenderStyle0", "ltb|..." );
	pList->AddTail( fxProp );

	fxProp.Path( "RenderStyle1", "ltb|..." );
	pList->AddTail( fxProp );

	fxProp.Path( "RenderStyle2", "ltb|..." );
	pList->AddTail( fxProp );

	fxProp.Path( "RenderStyle3", "ltb|..." );
	pList->AddTail( fxProp );

	fxProp.Combo( "SyncToModelAnim", "0,No,Yes" );
	pList->AddTail( fxProp );

	fxProp.Combo( "SyncToKey", "0,No,Yes" );
	pList->AddTail( fxProp );
}