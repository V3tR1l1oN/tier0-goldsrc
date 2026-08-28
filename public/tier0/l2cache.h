// tier0 -- clean-room functional reconstruction of GoldSrc tier0.dll (MIT).
//
// Purpose: Header for the CL2Cache class (PME L2-cache-miss profiler).
//
// $NoKeywords: $
//
//=============================================================================//

#ifndef L2CACHE_H
#define L2CACHE_H

class CL2Cache
{
public:
	CL2Cache();
	~CL2Cache();

	void Start();
	void End();

	inline int GetL2CacheMisses()
	{
		return m_nMisses;
	}

private:
	int m_nMisses;
	bool m_bEnabled;
};

extern CL2Cache g_L2Cache;

#endif // L2CACHE_H
