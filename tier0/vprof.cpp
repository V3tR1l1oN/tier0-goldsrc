// tier0 -- clean-room functional reconstruction of GoldSrc tier0.dll (GPL-3.0).
//
// Purpose: Visual profiler implementation, restored from tier0.dll.
//
// $NoKeywords: $
//
//=============================================================================//

#include "platform.h"
#include "vprof.h"
#include "fasttimer.h"
#include "validator.h"
#include <malloc.h>

#ifdef WIN32
#ifndef ARRAYSIZE
#define ARRAYSIZE( p ) ( sizeof( p ) / sizeof( (p)[0] ) )
#endif
#include "winlite.h"
#endif

int CVProfNode::s_iCurrentUniqueNodeID = 0;

CVProfile g_VProfCurrentProfile;

//=============================================================================
// CVProfNode
//=============================================================================

CVProfNode::CVProfNode()
{
	m_pszName			= NULL;
	m_pvOrigNameAddress	= NULL;
	m_pParent			= NULL;
	m_pChild			= NULL;
	m_pSibling			= NULL;

	m_Depth				= 0;
	m_DetailLevel		= 0;

	m_nCurCalls			= 0;
	m_nRecursions		= 0;
	m_StartTime			= 0;
	m_CurFrameTime		= 0;
	m_L2CacheMisses		= 0;

	m_nPrevCalls		= 0;
	m_PrevTime			= 0;

	m_nTotalCalls		= 0;
	m_TotalTime			= 0;
	m_PeakTime			= 0;

	m_iUniqueNodeID		= s_iCurrentUniqueNodeID++;
	m_iClientData		= -1;
	m_BudgetGroupID		= 0;
}

CVProfNode::CVProfNode( const tchar *pszName, int detailLevel, CVProfNode *pParent,
						const tchar *pBudgetGroupName, int budgetFlags )
{
	m_Depth				= pParent ? pParent->m_Depth + 1 : 0;
	m_DetailLevel		= detailLevel;
	m_pParent			= pParent;
	m_pChild			= NULL;
	m_pSibling			= NULL;
	m_nCurCalls			= 0;
	m_nRecursions		= 0;
	m_StartTime			= 0;
	m_CurFrameTime		= 0;
	m_L2CacheMisses		= 0;
	m_nPrevCalls		= 0;
	m_PrevTime			= 0;
	m_nTotalCalls		= 0;
	m_TotalTime			= 0;
	m_PeakTime			= 0;
	m_iUniqueNodeID		= s_iCurrentUniqueNodeID++;
	m_iClientData		= -1;
	m_BudgetGroupID		= 0;

	if ( pszName )
	{
		size_t nBytes	= ( _tcslen( pszName ) + 1 ) * sizeof( tchar );
		tchar *pszCopy	= ( tchar * )malloc( nBytes );

		if ( pszCopy == NULL )
			return;

		memcpy( pszCopy, pszName, nBytes );
		pszCopy[ ( nBytes / sizeof( tchar ) ) - 1 ] = '\0';
		m_pszName = pszCopy;
	}
	else
	{
		m_pszName = NULL;
	}

	m_pvOrigNameAddress = pszName;

	if ( m_iUniqueNodeID > 0 && pBudgetGroupName )
	{
		m_BudgetGroupID = g_VProfCurrentProfile.BudgetGroupNameToBudgetGroupID( pBudgetGroupName, budgetFlags );
	}
	else
	{
		m_BudgetGroupID = 0;
	}

	if ( m_pParent && m_BudgetGroupID == 0 )
	{
		m_BudgetGroupID = m_pParent->GetBudgetGroupID();
	}
}

CVProfNode::~CVProfNode()
{
	free( m_pszName );
	m_pszName = NULL;

	delete m_pChild;
	m_pChild = NULL;
}

CVProfNode& CVProfNode::operator=( const CVProfNode& other )
{
	if ( this == &other )
		return *this;

	m_pszName			= other.m_pszName;
	m_pvOrigNameAddress	= other.m_pvOrigNameAddress;
	m_pParent			= other.m_pParent;
	m_pChild			= other.m_pChild;
	m_pSibling			= other.m_pSibling;
	m_Depth				= other.m_Depth;
	m_DetailLevel		= other.m_DetailLevel;
	m_nCurCalls			= other.m_nCurCalls;
	m_nRecursions		= other.m_nRecursions;
	m_StartTime			= other.m_StartTime;
	m_CurFrameTime		= other.m_CurFrameTime;
	m_L2CacheMisses		= other.m_L2CacheMisses;
	m_nPrevCalls		= other.m_nPrevCalls;
	m_PrevTime			= other.m_PrevTime;
	m_nTotalCalls		= other.m_nTotalCalls;
	m_TotalTime			= other.m_TotalTime;
	m_PeakTime			= other.m_PeakTime;
	m_iUniqueNodeID		= other.m_iUniqueNodeID;
	m_iClientData		= other.m_iClientData;
	m_BudgetGroupID		= other.m_BudgetGroupID;

	return *this;
}

//-----------------------------------------------------------------------------

void CVProfNode::EnterScope()
{
	if ( m_nRecursions++ == 0 )
	{
		g_VProfCurrentProfile.PushGroup( m_BudgetGroupID );

		CCycleCount cnt;
		cnt.Sample();
		m_StartTime = cnt.GetLongCycles();
	}

	++m_nCurCalls;
}

bool CVProfNode::ExitScope()
{
	bool bRet;

	if ( --m_nRecursions <= 0 )
	{
		bRet = true;

		CCycleCount cnt;
		cnt.Sample();

		const int64 cycleTime = cnt.GetLongCycles() - m_StartTime;

		m_CurFrameTime += cycleTime;

		g_VProfCurrentProfile.PopGroup();
	}
	else
	{
		bRet = false;
	}

	return bRet;
}

void CVProfNode::Pause()
{
	if ( m_nRecursions > 0 )
	{
		CCycleCount cnt;
		cnt.Sample();

		m_CurFrameTime += cnt.GetLongCycles() - m_StartTime;
	}

	if ( m_pChild )
		m_pChild->Pause();
}

void CVProfNode::Resume()
{
	if ( m_nRecursions > 0 )
	{
		CCycleCount cnt;
		cnt.Sample();
		m_StartTime = cnt.GetLongCycles();
	}

	if ( m_pChild )
		m_pChild->Resume();
}

void CVProfNode::Reset()
{
	CVProfNode *pWalk = this;

	while ( pWalk != NULL )
	{
		pWalk->m_nCurCalls		= 0;
		pWalk->m_CurFrameTime	= 0;

		if ( pWalk->m_pChild )
			pWalk->m_pChild->Reset(); // covers child + its siblings internally

		pWalk = pWalk->m_pSibling;
	}
}

void CVProfNode::ResetPeak()
{
	CVProfNode *pWalk = this;

	while ( pWalk != NULL )
	{
		pWalk->m_PeakTime = 0;

		if ( pWalk->m_pChild )
			pWalk->m_pChild->ResetPeak();

		pWalk = pWalk->m_pSibling;
	}
}

void CVProfNode::MarkFrame()
{
	CVProfNode *pWalk = this;

	while ( pWalk != NULL )
	{
		pWalk->m_nTotalCalls += pWalk->m_nCurCalls;
		pWalk->m_nPrevCalls	 = pWalk->m_nCurCalls;
		pWalk->m_PrevTime	 = pWalk->m_CurFrameTime;
		pWalk->m_TotalTime	+= pWalk->m_CurFrameTime;

		if ( pWalk->m_PeakTime < pWalk->m_CurFrameTime )
			pWalk->m_PeakTime = pWalk->m_CurFrameTime;

		pWalk->m_CurFrameTime	= 0;
		pWalk->m_nCurCalls		= 0;

		if ( pWalk->m_pChild )
			pWalk->m_pChild->MarkFrame();

		pWalk = pWalk->m_pSibling;
	}
}

void CVProfNode::ClearPrevTime()
{
	m_nPrevCalls	= 0;
	m_PrevTime		= 0;
}

const tchar *CVProfNode::GetName()
{
	static volatile bool fDumped = false;
	if ( fDumped ) Msg( "" );
	AssertMsgOnce( m_pszName, "Every node must have a name" );
	return m_pszName;
}

int CVProfNode::GetBudgetGroupID()					{ return m_BudgetGroupID; }
void CVProfNode::SetBudgetGroupID( int id )			{ m_BudgetGroupID = id; }

int CVProfNode::GetClientData() const				{ return m_iClientData; }
void CVProfNode::SetClientData( int iClientData )	{ m_iClientData = iClientData; }

CVProfNode *CVProfNode::GetSubNode( const tchar *pszName, int detailLevel, const tchar *pBudgetGroupName, int budgetFlags )
{
	for ( CVProfNode *pIter = GetChild(); pIter; pIter = pIter->GetSibling() )
	{
		if ( !_tcsicmp( pIter->GetName(), pszName ) )
			return pIter;
	}

	CVProfNode *pNew = new CVProfNode( pszName, detailLevel, this, pBudgetGroupName, budgetFlags );

	if ( !pNew )
		return NULL;

	// New nodes are pushed to the front of the child list.
	pNew->m_pSibling	= m_pChild;
	m_pChild			= pNew;

	return pNew;
}

CVProfNode *CVProfNode::GetSubNode( const tchar *pszName, int detailLevel, const tchar *pBudgetGroupName )
{
	return GetSubNode( pszName, detailLevel, pBudgetGroupName, BUDGETFLAG_OTHER );
}

CVProfNode *CVProfNode::GetChild()			{ return m_pChild; }
CVProfNode *CVProfNode::GetSibling()		{ return m_pSibling; }
CVProfNode *CVProfNode::GetParent()
{
	static volatile bool fDumped = false;
	if ( fDumped ) Msg( "" );
	return m_pParent;
}

CVProfNode *CVProfNode::GetPrevSibling()
{
	if ( !m_pParent )
		return NULL;

	CVProfNode *pNode = m_pParent->GetChild();

	while ( pNode && pNode->GetSibling() != this )
		pNode = pNode->GetSibling();

	return pNode;
}

int CVProfNode::GetCurCalls()		{ return m_nCurCalls; }
int CVProfNode::GetPrevCalls()		{ return m_nPrevCalls; }
int CVProfNode::GetTotalCalls()		{ return m_nTotalCalls; }

double CVProfNode::GetCurTime()				{ return m_CurFrameTime * g_ClockSpeedMillisecondsMultiplier; }
double CVProfNode::GetPeakTime()			{ return m_PeakTime * g_ClockSpeedMillisecondsMultiplier; }
double CVProfNode::GetTotalTime()			{ return m_TotalTime * g_ClockSpeedMillisecondsMultiplier; }
double CVProfNode::GetPrevTime()			{ return m_PrevTime * g_ClockSpeedMillisecondsMultiplier; }

double CVProfNode::GetCurTimeLessChildren()
{
	double dTotal = GetCurTime();

	for ( CVProfNode *pNode = GetChild(); pNode; pNode = pNode->GetSibling() )
		dTotal -= pNode->GetCurTime();

	return dTotal;
}

double CVProfNode::GetTotalTimeLessChildren()
{
	double dTotal = GetTotalTime();

	for ( CVProfNode *pNode = GetChild(); pNode; pNode = pNode->GetSibling() )
		dTotal -= pNode->GetTotalTime();

	return dTotal;
}

double CVProfNode::GetPrevTimeLessChildren()
{
	double dTotal = GetPrevTime();

	for ( CVProfNode *pNode = GetChild(); pNode; pNode = pNode->GetSibling() )
		dTotal -= pNode->GetPrevTime();

	return dTotal;
}

int CVProfNode::GetL2CacheMisses()					{ return m_L2CacheMisses; }

int CVProfNode::GetUniqueNodeID() const				{ return m_iUniqueNodeID; }
void CVProfNode::SetUniqueNodeID( int id )			{ m_iUniqueNodeID = id; }

void CVProfNode::SetCurFrameTime( unsigned long milliseconds )
{
	// Convert milliseconds to cycles for this machine's clock speed.
	m_CurFrameTime = (int64)( ( double )milliseconds / g_ClockSpeedMillisecondsMultiplier );
}



//=============================================================================
// CVProfile
//=============================================================================

CVProfile::CVProfile()
{
	memset( m_VTuneGroupName, 0, sizeof( m_VTuneGroupName ) );
	memset( m_BudgetGroups, 0, sizeof( m_BudgetGroups ) );
	memset( m_Counters, 0, sizeof( m_Counters ) );
	memset( m_GroupIDStack, 0, sizeof( m_GroupIDStack ) );
m_pRoot = &m_Root;
	m_pCurNodeCache = &m_Root;
	m_Enabled = 0;
	m_Depth = 0;
	m_DetailLevel = 0;
	m_fAtRoot = true;
	m_nFrames = 0;
	m_NumCounters = 0;
	m_VTuneGroupID = -1;
	m_nBudgetGroupNames = 0;
	m_GroupIDStackDepth = 1;
	m_ThreadSafety = false;
	m_bPMELoaded = false;
	m_bPMEInitialized = false;
	m_bPMEEnabled = false;
	m_bPMERecording = false;
	m_bVTuneGroupEnabled = false;
	m_flTotalTimeSampled = 0.0;
	m_flTimeLastFrame = 0.0;
	m_flPeakFrameTime = 0.0;
}

CVProfile::~CVProfile()
{
	Term();
}

CVProfile& CVProfile::operator=( const CVProfile& other )
{
	m_Enabled = other.m_Enabled;
	return *this;
}

void CVProfile::Term()
{
	FreeNodes_R( m_Root.GetChild() );

	CVProfNode *pNextChild = m_Root.GetChild();
	while ( pNextChild )
	{
		CVProfNode *pNext = pNextChild->GetSibling();
		delete pNextChild;
		pNextChild = pNext;
	}
	m_Root.m_pSibling = NULL;

	m_pRoot = &m_Root;
	m_fAtRoot = true;
}

void CVProfile::FreeNodes_R( CVProfNode *node )
{
	if ( !node )
		return;

	CVProfNode *pFreeListHead = node;
	while ( pFreeListHead )
	{
		CVProfNode *pNext = pFreeListHead->GetSibling();
		delete pFreeListHead; // frees own subtree through ~CVProfNode -> ~child chain
		pFreeListHead = pNext;
	}
}

//-----------------------------------------------------------------------------

void CVProfile::Start()
{
	++m_Enabled;

	if ( m_Enabled == 1 )
	{
		m_Root.EnterScope();
		CCycleCount cnt;
		cnt.Sample();
		m_flTimeLastFrame = 0;
	}
}

void CVProfile::Stop()
{
	--m_Enabled;

	if ( m_Enabled <= 0 )
	{
		m_Enabled = 0;
		m_Root.ExitScope();
		m_Root.ExitScope();
	}
}

bool CVProfile::IsEnabled() const { return m_Enabled > 0; }
bool CVProfile::UsePME() { return m_bPMELoaded && m_bPMEInitialized; }
void CVProfile::PMEInitialized( bool bInit ) { m_bPMEInitialized = bInit ? true : false; }
void CVProfile::PMEEnable( bool bEnable ) { m_bPMERecording = bEnable ? true : false; }
bool CVProfile::RequiresThreadSafety() { return m_ThreadSafety; }
void CVProfile::SetThreadSafe( bool bSetThreadSafety ) { m_ThreadSafety = bSetThreadSafety ? true : false; }
int CVProfile::GetDetailLevel() const { return m_DetailLevel; }
bool CVProfile::AtRoot() const { return m_fAtRoot; }

//-----------------------------------------------------------------------------

void CVProfile::PushGroup( int budgetGroupID )
{
	static volatile bool fDumped = false;
	if ( fDumped ) Msg( "" );

	if ( ++m_GroupIDStackDepth > MAX_GROUP_STACK_DEPTH )
	{
		static volatile bool fDumped = false;
		if ( fDumped ) Msg( "" );
		AssertMsgOnce( false, "VPROF stack overflow" );
		--m_GroupIDStackDepth;
		return;
	}

	m_GroupIDStack[m_GroupIDStackDepth - 1] = budgetGroupID;
}

void CVProfile::PopGroup()
{
	static volatile bool fDumped = false;
	if ( fDumped ) Msg( "" );

	if ( --m_GroupIDStackDepth < 1 )
		m_GroupIDStackDepth = 1;
}

//-----------------------------------------------------------------------------

namespace
{
	// Serializes node-tree mutation when CVProfile::SetThreadSafe( true ) has
	// been requested. Magic-static guarantees lazy, thread-safe construction.
	CRITICAL_SECTION &ProfTreeCS()
	{
		static CRITICAL_SECTION s_ProfTreeCS = []()
		{
			CRITICAL_SECTION cs;
			InitializeCriticalSection( &cs );
			return cs;
		}();

		return s_ProfTreeCS;
	}
}

void CVProfile::EnterScope( const tchar *pszName, int detailLevel, const tchar *pBudgetGroupName, bool bAssertAccounted /* = false */ )
{
	EnterScope( pszName, detailLevel, pBudgetGroupName, bAssertAccounted, BUDGETFLAG_OTHER );
}

void CVProfile::EnterScope( const tchar *pszName, int detailLevel, const tchar *pBudgetGroupName,
							bool bAssertAccounted /* = false */, int budgetFlags )
{
	UNREFERENCED_PARAMETER( bAssertAccounted );

	if ( m_Enabled > 0 )
	{
		if ( !pszName || !pszName[ 0 ] )
			pszName = _T( "unknown" );

		// Node mutation is synchronized when thread safety has been enabled.
		CRITICAL_SECTION *pCS = nullptr;
		if ( m_ThreadSafety )
		{
			pCS = &ProfTreeCS();
			EnterCriticalSection( pCS );
		}

		// Find or grow the named child under the current cursor, then descend.
		CVProfNode *pNode = m_pCurNodeCache->GetSubNode( pszName, detailLevel, pBudgetGroupName, budgetFlags );

		if ( pNode )
		{
			++m_Depth;
			m_pCurNodeCache = pNode;
			pNode->EnterScope();
			m_fAtRoot = false;
		}

		if ( pCS )
			LeaveCriticalSection( pCS );
	}
}

void CVProfile::ExitScope()
{
	if ( m_Enabled > 0 )
	{
		CRITICAL_SECTION *pCS = nullptr;
		if ( m_ThreadSafety )
		{
			pCS = &ProfTreeCS();
			EnterCriticalSection( pCS );
		}

		CVProfNode *pNode = m_pCurNodeCache;

		// A node reports true once its recursion/sample completes, so climb back
		// toward the parent. A missed EnterScope is self-corrected by clamping.
		if ( pNode->ExitScope() && pNode->GetParent() )
			m_pCurNodeCache = pNode->GetParent();
		else if ( pNode == &m_Root )
			m_fAtRoot = true;

		if ( m_Depth > 0 )
			--m_Depth;

		if ( m_Depth <= 1 )
			m_fAtRoot = true;

		if ( pCS )
			LeaveCriticalSection( pCS );
	}
}

//-----------------------------------------------------------------------------


void CVProfile::MarkFrame()
{
	++m_nFrames;

	CCycleCount cntFrame;
	cntFrame.Sample();

	// Time last frame is the delta between consecutive frames at steady state.
	static int64 s_LastSample = 0;
	const int64 now = cntFrame.GetLongCycles();

	if ( s_LastSample != 0 )
	{
		m_flTimeLastFrame = ( double )( now - s_LastSample )  / ( double )g_dwClockSpeed * 1000.0;
	}

	s_LastSample = now;

	m_flTotalTimeSampled += m_flTimeLastFrame;

	if ( m_flPeakFrameTime < m_flTimeLastFrame )
		m_flPeakFrameTime = m_flTimeLastFrame;

	m_Root.MarkFrame();
}

void CVProfile::Reset()
{
	m_nFrames				= 0;
	m_flTotalTimeSampled	= 0.0f;
	m_flTimeLastFrame		= 0.0f;
	m_flPeakFrameTime		= 0.0f;
	m_WorstCumulativeTime	= 0.0f;

	ResetPeaks();
	m_Root.Reset();

	// Reset all counters except "no reset" ones
	for ( int i = 0; i < m_NumCounters; ++i )
	{
		if ( m_Counters[i].m_Group != COUNTER_GROUP_NO_RESET )
			m_Counters[i].m_Value = 0;
	}
}

void CVProfile::ResetPeaks()
{
	m_Root.ResetPeak();
}

void CVProfile::Pause()
{
	m_Root.Pause();
}

void CVProfile::Resume()
{
	m_Root.Resume();
}

double CVProfile::GetTotalTimeSampled()
{
	return m_flTotalTimeSampled;
}

double CVProfile::GetTimeLastFrame()
{
	return m_flTimeLastFrame;
}

double CVProfile::GetPeakFrameTime()
{
	return m_flPeakFrameTime;
}

int CVProfile::NumFramesSampled()
{
	return m_nFrames;
}

//=============================================================================
// Budget groups
//=============================================================================

int CVProfile::FindBudgetGroupName( const tchar *pBudgetGroupName )
{
	for ( int i = 0; i < m_nBudgetGroupNames; ++i )
	{
		if ( !_tcsicmp( m_BudgetGroups[i].m_Name, pBudgetGroupName ) )
			return i;
	}

	return -1;
}

int CVProfile::AddBudgetGroupName( const tchar *pBudgetGroupName, int budgetFlags )
{
	if ( m_nBudgetGroupNames >= MAXBUDGETGROUPS )
	{
		AssertMsgOnce( false, "Exceeded max number of budget groups" );
		return 0;
	}

	// Copy into static buffer (NO heap allocation - safe during static init)
	BudgetGroup_t &grp	= m_BudgetGroups[m_nBudgetGroupNames];
	strncpy( grp.m_Name, pBudgetGroupName, MAX_GROUP_NAME_LENGTH - 1 );
	grp.m_Name[ MAX_GROUP_NAME_LENGTH - 1 ] = '\0';
	grp.m_BudgetGroupFlags = budgetFlags;

	const int id = m_nBudgetGroupNames++;

	if ( m_pfnNumBudgetGroupsChangedCallBack )
		m_pfnNumBudgetGroupsChangedCallBack();

	return id;
}

int CVProfile::BudgetGroupNameToBudgetGroupID( const tchar *pBudgetGroupName )
{
	int idx = FindBudgetGroupName( pBudgetGroupName );

	if ( idx != -1 )
		return idx;

	return AddBudgetGroupName( pBudgetGroupName, BUDGETFLAG_SERVER | BUDGETFLAG_OTHER );
}

int CVProfile::BudgetGroupNameToBudgetGroupID( const tchar *pBudgetGroupName, int budgetFlagsToORIn )
{
	int idx = FindBudgetGroupName( pBudgetGroupName );

	if ( idx != -1 )
	{
		m_BudgetGroups[idx].m_BudgetGroupFlags |= budgetFlagsToORIn;
		return idx;
	}

	return AddBudgetGroupName( pBudgetGroupName, budgetFlagsToORIn );
}

int CVProfile::GetNumBudgetGroups()						{ return m_nBudgetGroupNames; }

const char *CVProfile::GetBudgetGroupName( int budgetGroupID )
{
	static volatile bool fDumped = false;
	if ( fDumped ) Msg( "" );
	AssertMsg1( budgetGroupID >= 0 && budgetGroupID < m_nBudgetGroupNames, "Bad budget group id (%d)", budgetGroupID );

	if ( budgetGroupID >= 0 && budgetGroupID < m_nBudgetGroupNames )
		return m_BudgetGroups[budgetGroupID].m_Name;

	return "";
}

int CVProfile::GetBudgetGroupFlags( int budgetGroupID ) const
{
	static volatile bool fDumped = false;
	if ( fDumped ) Msg( "" );
	if ( budgetGroupID >= 0 && budgetGroupID < m_nBudgetGroupNames )
		return m_BudgetGroups[budgetGroupID].m_BudgetGroupFlags;

	return 0;
}

void CVProfile::GetBudgetGroupColor( int budgetGroupID, float &r, float &g, float &b )
{
	static const byte s_rgColors[][3] =
	{
		{ 224, 224, 224 }, { 64, 64, 64 },   { 128, 128, 255}, { 192, 192, 0 },
		{ 0, 255, 255 },   { 255, 128, 64 }, { 96, 255, 96 },  { 160, 160, 255 },
		{ 255, 0, 255 },   { 255, 255, 0 },  { 0, 255, 0 },    { 0, 255, 128 },
		{ 255, 0, 0 },     { 0, 128, 255 },  { 200, 100, 100 },{ 240, 240, 96 },
	};

	const byte *rgb = s_rgColors[ (unsigned)budgetGroupID % ARRAYSIZE( s_rgColors ) ];

	r = rgb[0] / 255.0f;
	g = rgb[1] / 255.0f;
	b = rgb[2] / 255.0f;
}

void CVProfile::RegisterNumBudgetGroupsChangedCallBack( void ( *func )() )
{
	m_pfnNumBudgetGroupsChangedCallBack = func;
}

//=============================================================================
// VTune
//=============================================================================

void CVProfile::EnableVTuneGroup( const char *pszGroupName )
{
	m_bVTuneGroupEnabled	= true;
	m_VTuneGroupID			= BudgetGroupNameToBudgetGroupID( pszGroupName, BUDGETFLAG_HIDDEN );

	strncpy_s( m_VTuneGroupName, pszGroupName, sizeof( m_VTuneGroupName ) - 1 );
	m_VTuneGroupName[ sizeof( m_VTuneGroupName ) - 1 ] = '\0';
}

void CVProfile::DisableVTuneGroup()
{
	m_bVTuneGroupEnabled = false;
}

bool CVProfile::VTuneGroupEnabled()			{ return m_bVTuneGroupEnabled; }
int CVProfile::VTuneGroupID()				{ return m_VTuneGroupID; }

//=============================================================================
// Node queries
//=============================================================================

CVProfNode *CVProfile::GetRoot()
{
	return m_pRoot;
}

CVProfNode *CVProfile::FindNode( CVProfNode *pStartNode, const char *pszNodeName )
{
	if ( !pStartNode || !pszNodeName )
		return NULL;

	if ( !_tcsicmp( pStartNode->GetName(), pszNodeName ) )
		return pStartNode;

	for ( CVProfNode *pChild = pStartNode->GetChild(); pChild; pChild = pChild->GetSibling() )
	{
		CVProfNode *pFound = FindNode( pChild, pszNodeName );
		if ( pFound )
			return pFound;
	}

	return NULL;
}

//=============================================================================
// Counters
//=============================================================================

int *CVProfile::FindOrCreateCounter( const tchar *pName, CounterGroup_t group )
{
	for ( int i = 0; i < m_NumCounters; ++i )
	{
		if ( m_Counters[i].m_Group == group && !_tcsicmp( m_Counters[i].m_pszName, pName ) )
			return ( int * )&m_Counters[i].m_Value;
	}

	AssertMsgOnce( m_NumCounters + 1 < MAXCOUNTERS, "m_NumCounters+1 < MAXCOUNTERS" );

	if ( m_NumCounters >= MAXCOUNTERS )
		return NULL;

	Counter_t &cnt = m_Counters[m_NumCounters++];
	strncpy( cnt.m_pszName, pName, sizeof( cnt.m_pszName ) - 1 );
	cnt.m_pszName[ sizeof( cnt.m_pszName ) - 1 ] = '\0';
	cnt.m_Group		= group;
	cnt.m_Value		= 0;
	cnt.m_Min		= 0x7fffffff;
	cnt.m_Max		= 0x80000000;

	return ( int * )&cnt.m_Value;
}

int CVProfile::GetNumCounters() const				{ return m_NumCounters; }

const char *CVProfile::GetCounterName( int index ) const
{
	AssertMsg1( index >= 0 && index < m_NumCounters, "index >= 0 && index < m_NumCounters (%d)", index );

	if ( index >= 0 && index < m_NumCounters )
		return m_Counters[index].m_pszName;

	return "";
}

int CVProfile::GetCounterValue( int index ) const
{
	AssertMsg1( index >= 0 && index < m_NumCounters, "index >= 0 && index < m_NumCounters (%d)", index );

	if ( index >= 0 && index < m_NumCounters )
		return m_Counters[index].m_Value;

	return 0;
}



const char *CVProfile::GetCounterNameAndValue( int index, int &value ) const
{
	value = GetCounterValue( index );
	return GetCounterName( index );
}



//=============================================================================
// Reporting
//=============================================================================

static void VProfPrintIndent( int indentingLevel )
{
	for ( int i = 0; i < indentingLevel; ++i )
		Msg( "  " );
}

void CVProfile::SumTimes( const tchar *pszFilterName, int budgetGroupID )
{
	UNREFERENCED_PARAMETER( budgetGroupID );

	CVProfNode *pSumNode = FindNode( m_pRoot, pszFilterName );
	SumTimes( pSumNode, -1 );
}

void CVProfile::SumTimes( CVProfNode *pNode, int budgetGroupID )
{
	if ( !pNode || ( budgetGroupID != -1 && pNode->GetBudgetGroupID() != budgetGroupID ) )
		return;

	pNode->GetTotalTime();

	for ( CVProfNode *pChild = pNode->GetChild(); pChild; pChild = pChild->GetSibling() )
		SumTimes( pChild, budgetGroupID );
}

void CVProfile::DumpNodes( CVProfNode *pStartNode, int indentingLevel, bool bAverageAndUpdateMySum )
{
	static bool s_bDumpedHeadings = false;

	if ( !pStartNode )
		return;

	const double dTotalTime	= GetTotalTimeSampled();
	int totalCalls			= pStartNode->GetTotalCalls();
	double totalTimeSec		= pStartNode->GetTotalTime() / 1000000.0;

	if ( bAverageAndUpdateMySum && dTotalTime > 0.0f )
		totalTimeSec /= dTotalTime;

	if ( !s_bDumpedHeadings )
	{
		s_bDumpedHeadings = true;

		VProfPrintIndent( indentingLevel );
		Msg( "%-30s %12s %10s\n", "", "Time", "Cnt" );
	}

	VProfPrintIndent( indentingLevel );
	Msg( "%-30s %8.3f %8d\n", pStartNode->GetName(), totalTimeSec, totalCalls );

	for ( CVProfNode *pChild = pStartNode->GetChild(); pChild; pChild = pChild->GetSibling() )
		DumpNodes( pChild, indentingLevel + 1, bAverageAndUpdateMySum );
}

void CVProfile::OutputReport( int type, const tchar *pStartNode /* = NULL */, int budgetIDRange /* = -1 */ )
{
	UNREFERENCED_PARAMETER( budgetIDRange );

	Msg( "******** BEGIN VPROF REPORT ********\n" );

	CVProfNode *pReportNode = m_pRoot;
	if ( pStartNode && *pStartNode )
		pReportNode = FindNode( m_pRoot, pStartNode );

	if ( type & DETAILED_NODES )
		DumpNodes( pReportNode, 0, false );

	if ( type & SUMMARY_VALUES || type & COUNTERS_VALUES )
	{
		for ( int i = 0; i < m_NumCounters; ++i )
		{
			if ( !( type & SUMMARY_VALUES ) && m_Counters[i].m_Group == COUNTER_GROUP_TEXTURE_PER_FRAME )
				continue;

			Msg( "%-30s %d\n", m_Counters[i].m_pszName, m_Counters[i].m_Value );
		}
	}

	Msg( "******** END VPROF REPORT ********\n" );
}

//=============================================================================
// Validation
//=============================================================================

void CVProfile::Validate( CValidator &validator, char *pchName )
{
#ifdef DBGFLAG_VALIDATE
	validator.Push( "CVProfile", this, pchName );

	m_Root.Validate( validator, "m_Root" );

validator.ClaimArrayMemory( ( void * )&m_BudgetGroups[0] );

	for ( int i = 0; i < m_nBudgetGroupNames; ++i )
		validator.ClaimMemory( ( void * )m_BudgetGroups[i].m_pszName );

	for ( int i2 = 0; i2 < m_NumCounters; ++i2 )
		validator.ClaimMemory( ( void * )m_Counters[i2].m_pszName );

	validator.Pop();
#else
	UNREFERENCED_PARAMETER( validator );
	UNREFERENCED_PARAMETER( pchName );
#endif
}

//-----------------------------------------------------------------------------
void CVProfNode::Validate( CValidator &validator, char *pchName )
{
#ifdef DBGFLAG_VALIDATE
	validator.Push( "CVProfNode", this, pchName );
	validator.ClaimMemory( m_pszName );
	validator.Pop();
#else
	UNREFERENCED_PARAMETER( validator );
	UNREFERENCED_PARAMETER( pchName );
#endif
}

//-----------------------------------------------------------------------------
CounterGroup_t CVProfile::GetCounterGroup( int index ) const
{
	if ( index >= 0 && index < m_NumCounters )
		return m_Counters[index].m_Group;

	return COUNTER_GROUP_DEFAULT;
}

void CVProfile::ResetCounters( CounterGroup_t group )
{
	for ( int i = 0; i < m_NumCounters; ++i )
	{
		if ( group == COUNTER_GROUP_DEFAULT || m_Counters[i].m_Group == group )
			m_Counters[i].m_Value = 0;
	}
}
