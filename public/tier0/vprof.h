// tier0 -- clean-room functional reconstruction of GoldSrc tier0.dll (GPL-3.0).
//
// Purpose: VProf interface. Signatures follow the mangled exports of the
//			shipped tier0.dll exactly (argument types, return types,
//			cv-qualifiers) so the .def ordinals bind 1:1.
//
// $NoKeywords: $
//
//=============================================================================//

#ifndef VPROF_H
#define VPROF_H

#include "platform.h"
#include "l2cache.h"
#include "dbg.h"

#define BUDGETFLAG_CLIENT		( 1 << 0 )
#define BUDGETFLAG_SERVER		( 1 << 1 )
#define BUDGETFLAG_OTHER		( 1 << 2 )
#define BUDGETFLAG_HIDDEN		( 1 << 3 )
#define BUDGETFLAG_CVAR			( 1 << 9 )

enum CounterGroup_t
{
	COUNTER_GROUP_DEFAULT				= 0,
	COUNTER_GROUP_NO_RESET				= 1,
	COUNTER_GROUP_TEXTURE_GLOBAL		= 2,
	COUNTER_GROUP_TEXTURE_PER_FRAME		= 3,
};

enum VProfReportType_t
{
	SUMMARY				= ( 1 << 0 ),
	SUMMARY_VALUES		= ( 1 << 1 ),
	DETAILED_NODES		= ( 1 << 2 ),
	HIERARCHICAL		= ( 1 << 3 ),
	SUMMARIZE			= ( 1 << 4 ),
	FILTER_BUDGET_GROUP	= ( 1 << 5 ),
	COUNTERS			= ( 1 << 6 ),
	COUNTERS_VALUES		= ( 1 << 7 ),
};

#define MAXCOUNTERS					1024
#define MAX_GROUP_STACK_DEPTH		1024
#define MAX_GROUP_NAME_LENGTH		48
#define MAXBUDGETGROUPS				128

class CValidator;

//-----------------------------------------------------------------------------

class PLATFORM_CLASS CVProfNode
{
	friend class CVProfile;

public:
	CVProfNode();
	CVProfNode( const tchar *pszName, int detailLevel, CVProfNode *pParent,
				const tchar *pBudgetGroupName, int budgetFlags );
	~CVProfNode();

	CVProfNode& operator=( const CVProfNode& );

	void EnterScope();
	bool ExitScope();

	void Pause();
	void Resume();
	void Reset();
	void ResetPeak();
	void MarkFrame();
	void ClearPrevTime();

	const tchar *GetName();

	int GetBudgetGroupID();
	void SetBudgetGroupID( int id );

	int GetClientData() const;
	void SetClientData( int iClientData );

	CVProfNode *GetSubNode( const tchar *pszName, int detailLevel, const tchar *pBudgetGroupName );
	CVProfNode *GetSubNode( const tchar *pszName, int detailLevel, const tchar *pBudgetGroupName, int budgetFlags );

	CVProfNode *GetChild();
	CVProfNode *GetSibling();
	CVProfNode *GetPrevSibling();
	CVProfNode *GetParent();

	int GetCurCalls();
	int GetPrevCalls();
	int GetTotalCalls();

	double GetCurTime();
	double GetCurTimeLessChildren();
	double GetPeakTime();
	double GetTotalTime();
	double GetTotalTimeLessChildren();
	double GetPrevTime();
	double GetPrevTimeLessChildren();

	int GetL2CacheMisses();

	const void *GetOrigNameAddress();

	void Validate( CValidator &validator, char *pchName );

private:
	int GetUniqueNodeID() const;
	void SetUniqueNodeID( int id );

public:
	

	void SetCurFrameTime( unsigned long milliseconds );

private:
	tchar *m_pszName;
	const char *m_pvOrigNameAddress;

	CVProfNode *m_pParent;
	CVProfNode *m_pChild;
	CVProfNode *m_pSibling;

	int m_Depth;
	int m_DetailLevel;

	int m_nCurCalls;
	int m_nRecursions;
	int64 m_StartTime;
	int64 m_CurFrameTime;
	int m_L2CacheMisses;

	int m_nPrevCalls;
	int64 m_PrevTime;

	int m_nTotalCalls;
	int64 m_TotalTime;

	int64 m_PeakTime;
	int m_iUniqueNodeID;

	int m_iClientData;
	int m_BudgetGroupID;

	CL2Cache m_L2Cache;
private:
	static int s_iCurrentUniqueNodeID;
};

//-----------------------------------------------------------------------------

class PLATFORM_CLASS CVProfile
{
	friend class CVProfNode;

public:
	CVProfile();
	~CVProfile();

	CVProfile& operator=( const CVProfile& );

	void Start();
	void Stop();
	void Term();

	bool IsEnabled() const;
	bool UsePME();
	void PMEInitialized( bool bInit );
	void PMEEnable( bool bEnable );

	bool RequiresThreadSafety();
	void SetThreadSafe( bool bSetThreadSafety );

	int GetDetailLevel() const;
	bool AtRoot() const;

	void PushGroup( int budgetGroupID );
	void PopGroup();

	void EnterScope( const tchar *pszName, int detailLevel, const tchar *pBudgetGroupName, bool bAssertAccounted );
	void EnterScope( const tchar *pszName, int detailLevel, const tchar *pBudgetGroupName, bool bAssertAccounted, int budgetFlags );
	void ExitScope();

	void MarkFrame();
	void Reset();
	void ResetPeaks();
	void Pause();
	void Resume();

	int NumFramesSampled();
	double GetTotalTimeSampled();
	double GetTimeLastFrame();
	double GetPeakFrameTime();

	int GetNumBudgetGroups();
	const char *GetBudgetGroupName( int budgetGroupID );
	int GetBudgetGroupFlags( int budgetGroupID ) const;
	void GetBudgetGroupColor( int budgetGroupID, float &r, float &g, float &b );
	void GetBudgetGroupColor( int budgetGroupID, int &r, int &g, int &b, int &a );

	int BudgetGroupNameToBudgetGroupID( const tchar *pBudgetGroupName );
	int BudgetGroupNameToBudgetGroupID( const tchar *pBudgetGroupName, int budgetFlagsToORIn );

	void RegisterNumBudgetGroupsChangedCallBack( void ( *func )() );

	void EnableVTuneGroup( const char *pszGroupName );
	void DisableVTuneGroup();
	protected:
	bool VTuneGroupEnabled();
public:
	protected:
	int VTuneGroupID();
public:

	CVProfNode *GetRoot();
	CVProfNode *FindNode( CVProfNode *pStartNode, const char *pszNodeName );

	int *FindOrCreateCounter( const tchar *pName, CounterGroup_t group );
	int GetNumCounters() const;
	const char *GetCounterName( int index ) const;
	int GetCounterValue( int index ) const;
	CounterGroup_t GetCounterGroup( int index ) const;
	const char *GetCounterNameAndValue( int index, int &value ) const;
	void ResetCounters( CounterGroup_t group );

	void OutputReport( int type = SUMMARY|DETAILED_NODES, const tchar *pStartNode = NULL, int budgetIDRange = -1 );

	public:
	void Validate( CValidator &validator, char *pchName );

protected:
	int FindBudgetGroupName( const tchar *pBudgetGroupName );
	int AddBudgetGroupName( const tchar *pBudgetGroupName, int budgetFlags );

	void SumTimes( const tchar *pszFilterName, int budgetGroupID );
	void SumTimes( CVProfNode *pNode, int budgetGroupID );
	void DumpNodes( CVProfNode *pStartNode, int indentingLevel, bool bAverageAndUpdateMySum );
	void FreeNodes_R( CVProfNode *node );

private:
	CVProfNode	m_Root;
	CVProfNode	*m_pRoot;

	int			m_Enabled;
	int			m_Depth;
	int			m_FrameCount;
	int			m_nFrames;
	int			m_DetailLevel;

	double		m_flTotalTimeSampled;
	double		m_flTimeLastFrame;
	double		m_flPeakFrameTime;
	double		m_WorstCumulativeTime;

	int			m_NumCounters;

	bool		m_fAtRoot;
	bool		m_bPMELoaded;
	bool		m_bPMERecording;
	bool		m_bPMEInitialized;
	bool		m_bPMEEnabled;
	bool		m_bVTuneGroupEnabled;
	bool		m_ThreadSafety;

	int			m_VTuneGroupID;
	char		m_VTuneGroupName[MAXBUDGETGROUPS];

public:
	struct BudgetGroup_t
	{
		char m_Name[MAX_GROUP_NAME_LENGTH];
		int m_BudgetGroupFlags;
	};

protected:
	BudgetGroup_t	m_BudgetGroups[MAXBUDGETGROUPS];
	int				m_nBudgetGroupNames;
	void			(*m_pfnNumBudgetGroupsChangedCallBack)( void );

	int				m_GroupIDStack[MAX_GROUP_STACK_DEPTH];
	int				m_GroupIDStackDepth;

	struct Counter_t
	{
		char m_pszName[64];
		CounterGroup_t m_Group;
		long m_Value;
		long m_Min;
		long m_Max;
	};

	Counter_t		m_Counters[MAXCOUNTERS];

	CVProfNode		*m_pCurNodeCache;
};

// Exported profile singleton.
extern PLATFORM_CLASS CVProfile g_VProfCurrentProfile;

#endif // VPROF_H
