/******************************************************************************
 *
 * MantaFlow fluid solver framework
 * Copyright 2011 Tobias Pfaff, Nils Thuerey 
 *
 * This program is free software, distributed under the terms of the
 * GNU General Public License (GPL) 
 * http://www.gnu.org/licenses
 *
 * Multigrid solver by Florian Ferstl (florian.ferstl.ff@gmail.com)
 *
 * This is an implementation of the solver developed by Dick et al. [1]
 * without topology awareness (= vertex duplication on coarser levels). This 
 * simplification allows us to use regular grids for all levels of the multigrid
 * hierarchy and works well for moderately complex domains.
 *
 * [1] Solving the Fluid Pressure Poisson Equation Using Multigrid-Evaluation
 *     and Improvements, C. Dick, M. Rogowsky, R. Westermann, IEEE TVCG 2015 
 *
 ******************************************************************************/

#include "multigrid.h"

#define FOR_LVL(IDX,LVL) \
	for(int IDX=0; IDX<mb[LVL].size(); IDX++)

#define FOR_VEC_LVL(VEC,LVL) Vec3i VEC; \
	for(VEC.z=0; VEC.z<mSize[LVL].z; VEC.z++) \
	for(VEC.y=0; VEC.y<mSize[LVL].y; VEC.y++) \
	for(VEC.x=0; VEC.x<mSize[LVL].x; VEC.x++)

#define FOR_VEC_MINMAX(VEC,MIN,MAX) Vec3i VEC; \
	const Vec3i VEC##__min = (MIN), VEC##__max = (MAX); \
	for(VEC.z=VEC##__min.z; VEC.z<=VEC##__max.z; VEC.z++) \
	for(VEC.y=VEC##__min.y; VEC.y<=VEC##__max.y; VEC.y++) \
	for(VEC.x=VEC##__min.x; VEC.x<=VEC##__max.x; VEC.x++)

#define FOR_VECLIN_MINMAX(VEC,LIN,MIN,MAX) Vec3i VEC; int LIN = 0; \
	const Vec3i VEC##__min = (MIN), VEC##__max = (MAX); \
	for(VEC.z=VEC##__min.z; VEC.z<=VEC##__max.z; VEC.z++) \
	for(VEC.y=VEC##__min.y; VEC.y<=VEC##__max.y; VEC.y++) \
	for(VEC.x=VEC##__min.x; VEC.x<=VEC##__max.x; VEC.x++, LIN++)

#define MG_TIMINGS(X)
//#define MG_TIMINGS(X) X

using namespace std;
namespace Manta 
{

// ----------------------------------------------------------------------------
// Efficient min heap for <ID, key> pairs with 0<=ID<N and 0<=key<K
// (elements are stored in K buckets, where each bucket is a doubly linked list).
// - if K<<N, all ops are O(1) on avg (worst case O(K)).
// - memory usage O(K+N): (K+N) * 3 * sizeof(int).
class NKMinHeap
{
private:
	struct Entry {
		int key, prev, next;
		Entry() : key(-1), prev(-1), next(-1) {}
	};

	int mN, mK, mSize, mMinKey;

	// Double linked lists of IDs, one for each bucket/key.
	// The first K entries are the buckets' head pointers,
	// and the last N entries correspond to the IDs.
	std::vector<Entry> mEntries; 
	
public:
	NKMinHeap(int N, int K) : mN(N), mK(K), mSize(0), mMinKey(-1), mEntries(N+K) {}

	int size() { return mSize; }
	int getKey(int ID) { return mEntries[mK+ID].key; }
	
	// Insert, decrease or increase key (or delete by setting key to -1)
	void setKey(int ID, int key);

	// peek min key (returns ID/key pair)
	std::pair<int,int> peekMin();

	// pop min key (returns ID/key pair)
	std::pair<int,int> popMin();
	
	void print(); // for debugging
};

void NKMinHeap::setKey(int ID, int key)
{
	assertMsg(0 <=ID  && ID <mN, "NKMinHeap::setKey: ID out of range");
	assertMsg(-1<=key && key<mK, "NKMinHeap::setKey: key out of range");

	const int kid = mK + ID;

	if (mEntries[kid].key == key) return; // nothing changes

	// remove from old key-list if ID existed previously
	if (mEntries[kid].key != -1)
	{
		int pred = mEntries[kid].prev;
		int succ = mEntries[kid].next; // can be -1

		mEntries[pred].next = succ;
		if (succ != -1) mEntries[succ].prev = pred;

		// if removed key was minimum key, mMinKey may need to be updated
		int removedKey = mEntries[kid].key;
		if (removedKey==mMinKey)
		{
			if (mSize==1) { mMinKey = -1; }
			else {
				for (; mMinKey<mK; mMinKey++) {
					if (mEntries[mMinKey].next != -1) break;
				}
			}
		}

		mSize--;
	}

	// set new key of ID
	mEntries[kid].key = key;

	if (key==-1) {
		// finished if key was set to -1
		mEntries[kid].next = mEntries[kid].prev = -1;
		return;
	}

	// add key
	mSize++;
	if (mMinKey == -1) mMinKey = key;
	else mMinKey = std::min(mMinKey, key);

	// insert into new key-list (headed by mEntries[key])
	int tmp = mEntries[key].next;

	mEntries[key].next = kid;
	mEntries[kid].prev = key;

	mEntries[kid].next = tmp;
	if (tmp != -1) mEntries[tmp].prev = kid;
}

std::pair<int,int> NKMinHeap::peekMin()
{
	if (mSize==0) return std::pair<int,int>(-1,-1); // error
		
	const int ID = mEntries[mMinKey].next - mK;
	return std::pair<int,int>(ID, mMinKey);
}

std::pair<int,int> NKMinHeap::popMin()
{
	if (mSize==0) return std::pair<int,int>(-1,-1); // error

	const int kid = mEntries[mMinKey].next;
	const int ID = kid - mK;
	const int key = mMinKey;

	// remove from key-list
	int pred = mEntries[kid].prev;
	int succ = mEntries[kid].next; // can be -1

	mEntries[pred].next = succ;
	if (succ != -1) mEntries[succ].prev = pred;

	// remove entry
	mEntries[kid] = Entry();	
	mSize--;

	// update mMinKey
	if (mSize==0) { mMinKey = -1; }
	else {
		for (; mMinKey<mK; mMinKey++) {
			if (mEntries[mMinKey].next != -1) break;
		}
	}
		
	// return result
	return std::pair<int,int>(ID, key);
}

void NKMinHeap::print()
{
	std::cout << "Size: "<<mSize<<", MinKey: "<<mMinKey<< std::endl;
	for (int key=0; key<mK; key++) {
		if (mEntries[key].next != -1) {
			std::cout << "Key "<<key<<": ";
			int kid = mEntries[key].next;
			while (kid != -1) {
				std::cout << kid-mK<<" ";
				kid = mEntries[kid].next;
			}
			std::cout << std::endl;
		}
	}
	std::cout << std::endl;
}

// ----------------------------------------------------------------------------
// GridMg methods
//
// Illustration of 27-point stencil indices
// y     | z = -1    z = 0      z = 1
// ^     | 6  7  8,  15 16 17,  24 25 26
// |     | 3  4  5,  12 13 14,  21 22 23
// o-> x | 0  1  2,   9 10 11,  18 19 20
//
// Symmetric storage with only 14 entries per vertex
// y     | z = -1    z = 0      z = 1
// ^     | -  -  -,   2  3  4,  11 12 13
// |     | -  -  -,   -  0  1,   8  9 10
// o-> x | -  -  -,   -  -  -,   5  6  7

GridMg::GridMg(const Vec3i& gridSize)
  : mA(),
	mNumPreSmooth(1),
	mNumPostSmooth(1),
	mCoarsestLevelAccuracy(1E-8),
	mTrivialEquationScale(1E-6),
	mIsASet(false),
	mIsRhsSet(false)
{
	MG_TIMINGS(MuTime time;)

	// 2D or 3D mode
	mIs3D = (gridSize.z > 1); 
	mDim = mIs3D ? 3 : 2; 
	mStencilSize  = mIs3D ? 14 : 5; // A has a full 27-point stencil on levels > 0
	mStencilSize0 = mIs3D ?  4 : 3; // A has a 7-point stencil on level 0
	mStencilMin = Vec3i(-1,-1, mIs3D ? -1:0);
	mStencilMax = Vec3i( 1, 1, mIs3D ?  1:0);

	// Create level 0 (=original grid)
	mSize.push_back(gridSize);
	mPitch.push_back(Vec3i(1, mSize.back().x, mSize.back().x*mSize.back().y));
	int n = mSize.back().x * mSize.back().y * mSize.back().z;

	mA.push_back(std::vector<Real>(n * mStencilSize0));
	mx.push_back(std::vector<Real>(n));
	mb.push_back(std::vector<Real>(n));
	mr.push_back(std::vector<Real>(n));
	mType.push_back(std::vector<VertexType>(n));
	mCGtmp1.push_back(std::vector<Real>());
	mCGtmp2.push_back(std::vector<Real>());

	debMsg("GridMg::GridMg level 0: "<<mSize[0].x<<" x " << mSize[0].y << " x " << mSize[0].z << " x ", 2);

	// Create coarse levels >0
	for (int l=1; l<=100; l++)
	{
		if (mSize[l-1].x <= 5 && mSize[l-1].y <= 5 && mSize[l-1].z <= 5) break;
		if (n <= 1000) break;

		mSize.push_back((mSize[l-1] + 2) / 2);
		mPitch.push_back(Vec3i(1, mSize.back().x, mSize.back().x*mSize.back().y));
		n = mSize.back().x * mSize.back().y * mSize.back().z;

		mA.push_back(std::vector<Real>(n * mStencilSize));
		mx.push_back(std::vector<Real>(n));
		mb.push_back(std::vector<Real>(n));
		mr.push_back(std::vector<Real>(n));
		mType.push_back(std::vector<VertexType>(n));
		mCGtmp1.push_back(std::vector<Real>());
		mCGtmp2.push_back(std::vector<Real>());
		
		debMsg("GridMg::GridMg level "<<l<<": " << mSize[l].x << " x " << mSize[l].y << " x " << mSize[l].z << " x ", 2);
	}

	// Additional memory for CG on coarsest level
	mCGtmp1.pop_back();
	mCGtmp1.push_back(std::vector<Real>(n));
	mCGtmp2.pop_back();
	mCGtmp2.push_back(std::vector<Real>(n));

	MG_TIMINGS(debMsg("GridMg: Allocation done in "<<time.update(), 1);)

	// Precalculate coarsening paths:
	// (V) <--restriction-- (U) <--A_{l-1}-- (W) <--interpolation-- (N)
	Vec3i p7stencil[7] = { Vec3i(0,0,0), Vec3i(-1, 0, 0), Vec3i(1,0,0),
	                                     Vec3i( 0,-1, 0), Vec3i(0,1,0),
	                                     Vec3i( 0, 0,-1), Vec3i(0,0,1) };
	Vec3i V (1,1,1); // reference coarse grid vertex at (1,1,1)
	FOR_VEC_MINMAX(U, V*2+mStencilMin, V*2+mStencilMax) {		
		for (int i=0; i<1+2*mDim; i++) {
			Vec3i W = U + p7stencil[i];
			FOR_VEC_MINMAX(N, W/2, (W+1)/2) {				
				int s = dot(N,Vec3i(1,3,9));

				if (s>=13) {
					CoarseningPath path;
					path.N  = N-1;   // offset of N on coarse grid
					path.U  = U-2*V; // offset of U on fine grid
					path.W  = W-2*V; // offset of W on fine grid
					path.sc = s-13;  // stencil index corresponding to V<-N on coarse grid
					path.sf = (i+1)/2;   // stencil index corresponding to U<-W on coarse grid
					path.inUStencil = (i%2==0); // fine grid stencil entry stored at U or W?
					path.rw = Real(1) / Real(1 << ((U.x % 2) + (U.y % 2) + (U.z % 2))); // restriction weight V<-U
					path.iw = Real(1) / Real(1 << ((W.x % 2) + (W.y % 2) + (W.z % 2))); // interpolation weight W<-N
					mCoarseningPaths0.push_back(path);
				}
			}
		}
	}

	auto pathLess = [](const GridMg::CoarseningPath& p1, const GridMg::CoarseningPath& p2) {
		if (p1.sc == p2.sc) return dot(p1.U+1,Vec3i(1,3,9)) < dot(p2.U+1,Vec3i(1,3,9));
		return p1.sc < p2.sc;
	};
	std::sort(mCoarseningPaths0.begin(), mCoarseningPaths0.end(), pathLess);
}

void GridMg::analyzeStencil(int v, bool& isStencilSumNonZero, bool& isEquationTrivial) {
	Vec3i V = vecIdx(v,0);

	// collect stencil entries
	Real A[7];
	A[0] = mA[0][v*mStencilSize0 + 0];
	A[1] = mA[0][v*mStencilSize0 + 1];
	A[2] = mA[0][v*mStencilSize0 + 2];
	A[3] = mA[0][v*mStencilSize0 + 3];
	A[4] = V.x==0 ? Real(0) : mA[0][(v-mPitch[0].x)*mStencilSize0 + 1];
	A[5] = V.y==0 ? Real(0) : mA[0][(v-mPitch[0].y)*mStencilSize0 + 2];
	A[6] = V.z==0 ? Real(0) : mA[0][(v-mPitch[0].z)*mStencilSize0 + 3];
	
	// compute sum of stencil entries
	Real stencilMax = Real(0), stencilSum = Real(0);
	for (int i=0; i<7; i++) {
		stencilSum += A[i];
		stencilMax = max(stencilMax, abs(A[i]));
	}

	// check if sum is numerically zero
	isStencilSumNonZero = abs(stencilSum / stencilMax) > Real(1E-6);

	// check for trivial equation (exact comparisons)
	isEquationTrivial = A[0]==Real(1) 
		             && A[1]==Real(0) && A[2]==Real(0) && A[3]==Real(0) 
	                 && A[4]==Real(0) && A[5]==Real(0) && A[6]==Real(0);
}

void GridMg::setA(Grid<Real>* A0, Grid<Real>* pAi, Grid<Real>* pAj, Grid<Real>* pAk)
{
	MG_TIMINGS(MuTime time;)


	// Copy level 0
	#pragma omp parallel for
	FOR_LVL(v,0) {
		mA[0][v*mStencilSize0 + 0] = (* A0)[v];
		mA[0][v*mStencilSize0 + 1] = (*pAi)[v];
		mA[0][v*mStencilSize0 + 2] = (*pAj)[v];
		mA[0][v*mStencilSize0 + 3] = mIs3D ? (*pAk)[v] : Real(0);		
	}
	
	// Determine active vertices and scale trivial equations
	bool nonZeroStencilSumFound = false;
	bool trivialEquationsFound  = false;

	#pragma omp parallel for
	FOR_LVL(v,0) {
		// active vertices on level 0 are vertices with non-zero diagonal entry in A
		mType[0][v] = vtInactive;
		
		if (mA[0][v*mStencilSize0 + 0] != Real(0)) {
			mType[0][v] = vtActive;

			bool isStencilSumNonZero = false, isEquationTrivial = false;
			analyzeStencil(v, isStencilSumNonZero, isEquationTrivial);
			
			if (isStencilSumNonZero) nonZeroStencilSumFound = true;

			// scale down trivial equations
			if (isEquationTrivial) { 
				mType[0][v] = vtActiveTrivial;
				mA[0][v*mStencilSize0 + 0] *= mTrivialEquationScale; 
				trivialEquationsFound = true;
			};

		}					
	}

	if (trivialEquationsFound)   debMsg("GridMg::setA: Found at least one trivial equation", 2);

	// Sanity check: if all rows of A sum up to 0 --> A doesn't have full rank (opposite direction isn't necessarily true)
    if (!nonZeroStencilSumFound) debMsg("GridMg::setA: Matrix doesn't have full rank, multigrid may not converge", 1);
	
	// Create coarse grids and operators on levels >0
	for (int l=1; l<mA.size(); l++) {
		MG_TIMINGS(time.get();)
		genCoarseGrid(l);	
		MG_TIMINGS(debMsg("GridMg: Generated level "<<l<<" in "<<time.update(), 1);)
		genCoraseGridOperator(l);	
		MG_TIMINGS(debMsg("GridMg: Generated operator "<<l<<" in "<<time.update(), 1);)
	}

	mIsASet   = true;
	mIsRhsSet = false; // invalidate rhs
}

void GridMg::setRhs(Grid<Real>& rhs)
{
	assertMsg(mIsASet, "GridMg::setRhs Error: A has not been set.");

	#pragma omp parallel for
	FOR_LVL(v,0)
	{
		mb[0][v] = rhs[v];

		// scale down trivial equations
		if (mType[0][v] == vtActiveTrivial) { mb[0][v] *= mTrivialEquationScale; };
	}

	mIsRhsSet = true;
}

Real GridMg::doVCycle(Grid<Real>& dst, Grid<Real>* src)
{
	MG_TIMINGS(MuTime timeSmooth; MuTime timeCG; MuTime timeI; MuTime timeR; MuTime timeTotal; MuTime time;)
	MG_TIMINGS(timeSmooth.clear(); timeCG.clear(); timeI.clear(); timeR.clear();)

	assertMsg(mIsASet && mIsRhsSet, "GridMg::doVCycle Error: A and/or rhs have not been set.");

	const int maxLevel = mA.size() - 1;

	#pragma omp parallel for
	FOR_LVL(v,0) { mx[0][v] = src ? (*src)[v] : Real(0); }

	for (int l=0; l<maxLevel; l++)
	{
		MG_TIMINGS(time.update();)
		for (int i=0; i<mNumPreSmooth; i++) {
			smoothGS(l, false);
		}
		
		MG_TIMINGS(timeSmooth += time.update();)

		calcResidual(l);
		restrict(l+1, mr[l], mb[l+1]);

		#pragma omp parallel for
		FOR_LVL(v,l+1) { mx[l+1][v] = Real(0); }

		MG_TIMINGS(timeR += time.update();)
	}

	MG_TIMINGS(time.update();)
	solveCG(maxLevel);
	MG_TIMINGS(timeCG += time.update();)

	for (int l=maxLevel-1; l>=0; l--)
	{
		MG_TIMINGS(time.update();)
		interpolate(l, mx[l+1], mr[l]);
		
		#pragma omp parallel for
		FOR_LVL(v,l) { mx[l][v] += mr[l][v]; }

		MG_TIMINGS(timeI += time.update();)

		for (int i=0; i<mNumPostSmooth; i++) {
			smoothGS(l, true);
		}
		MG_TIMINGS(timeSmooth += time.update();)
	}

	calcResidual(0);
	Real res = calcResidualNorm(0);

	#pragma omp parallel for
	FOR_LVL(v,0) { dst[v] = mx[0][v];	}

	MG_TIMINGS(debMsg("GridMg: Finished VCycle in "<<timeTotal.update()<<" (smoothing: "<<timeSmooth<<", CG: "<<timeCG<<", R: "<<timeR<<", I: "<<timeI<<")", 1);)

	return res;
}


// Determine active cells on coarse level l from active cells on fine level l-1
// while ensuring a full-rank interpolation operator (see Section 3.3 in [1]).
void GridMg::genCoarseGrid(int l)
{
	//    AF_Free: unused/untouched vertices
	//    AF_Zero: vertices selected for coarser level
	// AF_Removed: vertices removed from coarser level
	enum activeFlags : char {AF_Removed = 0, AF_Zero = 1, AF_Free = 2};

	// initialize all coarse vertices with 'free'
	#pragma omp parallel for
	FOR_LVL(v,l) { mType[l][v] = vtFree; }

	// initialize min heap of (ID: fine grid vertex, key: #free interpolation vertices) pairs
	NKMinHeap heap(mb[l-1].size(), mIs3D ? 9 : 5); // max 8 (or 4 in 2D) free interpolation vertices
		
	FOR_LVL(v,l-1) {
		if (mType[l-1][v] != vtInactive) {
			Vec3i V = vecIdx(v,l-1);
			int fiv = 1 << ((V.x % 2) + (V.y % 2) + (V.z % 2));
			heap.setKey(v, fiv);
		}
	}

	// process fine vertices in heap consecutively, always choosing the vertex with 
	// the currently smallest number of free interpolation vertices
	while (heap.size() > 0)
	{
		int v = heap.popMin().first;
		Vec3i V = vecIdx(v,l-1);

		// loop over associated interpolation vertices of V on coarse level l:
		// the first encountered 'free' vertex is set to 'zero',
		// all remaining 'free' vertices are set to 'removed'.
		bool vdone = false;

		FOR_VEC_MINMAX(I, V/2, (V+1)/2) {
			int i = linIdx(I,l);

			if (mType[l][i] == vtFree) {
				if (vdone) {
					mType[l][i] = vtRemoved; 
				} else {
					mType[l][i] = vtZero; 
					vdone = true;
				}

				// update #free interpolation vertices in heap:
				// loop over all associated restriction vertices of I on fine level l-1
				FOR_VEC_MINMAX(R, vmax(0, I*2-1), vmin(mSize[l-1]-1, I*2+1)) {
					int r = linIdx(R,l-1);
					int key = heap.getKey(r); 

					if      (key > 1) { heap.setKey(r, key-1); } // decrease key of r
					else if (key >-1) { heap.setKey(r, -1); } // removes r from heap
				}
			}
		}
	}

	#pragma omp parallel for
	FOR_LVL(v,l) { 
		// set all remaining 'free' vertices to 'removed',
		if (mType[l][v] == vtFree) mType[l][v] = vtRemoved;

		// then convert 'zero' vertices to 'active' and 'removed' vertices to 'inactive'
		if (mType[l][v] == vtZero   ) mType[l][v] = vtActive;
		if (mType[l][v] == vtRemoved) mType[l][v] = vtInactive;
	}
}

// Calculate A_l on coarse level l from A_{l-1} on fine level l-1 using 
// Galerkin-based coarsening, i.e., compute A_l = R * A_{l-1} * I.
void GridMg::genCoraseGridOperator(int l)
{
	// loop over coarse grid vertices V
	#pragma omp parallel for schedule(static,1)
	FOR_LVL(v,l) {
		if (mType[l][v] == vtInactive) continue;

		for (int i=0; i<mStencilSize; i++) { mA[l][v*mStencilSize+i] = Real(0); } // clear stencil

		Vec3i V = vecIdx(v,l);

		// Calculate the stencil of A_l at V by considering all vertex paths of the form:
		// (V) <--restriction-- (U) <--A_{l-1}-- (W) <--interpolation-- (N)
		// V and N are vertices on the coarse grid level l, 
		// U and W are vertices on the fine grid level l-1.

		if (l==1) {
			// loop over precomputed paths
			for (auto it = mCoarseningPaths0.begin(); it != mCoarseningPaths0.end(); it++) {
				Vec3i N = V + it->N;
				int n = linIdx(N,l);
				if (!inGrid(N,l) || mType[l][n]==vtInactive) continue;

				Vec3i U = V*2 + it->U;
				int u = linIdx(U,l-1);
				if (!inGrid(U,l-1) || mType[l-1][u]==vtInactive) continue;

				Vec3i W = V*2 + it->W;
				int w = linIdx(W,l-1);
				if (!inGrid(W,l-1) || mType[l-1][w]==vtInactive) continue;
				
				if (it->inUStencil) {
					mA[l][v*mStencilSize + it->sc] += it->rw * mA[l-1][u*mStencilSize0 + it->sf] *it->iw;			
				} else {
					mA[l][v*mStencilSize + it->sc] += it->rw * mA[l-1][w*mStencilSize0 + it->sf] *it->iw;			
				}
			}
		} else {
			// l > 1: 
			// loop over restriction vertices U on level l-1 associated with V
			FOR_VEC_MINMAX(U, vmax(0, V*2-1), vmin(mSize[l-1]-1, V*2+1)) {
				int u = linIdx(U,l-1);
				if (mType[l-1][u] == vtInactive) continue;

				// restriction weight			
				Real rw = Real(1) / Real(1 << ((U.x % 2) + (U.y % 2) + (U.z % 2))); 

				// loop over all stencil neighbors N of V on level l that can be reached via restriction to U
				FOR_VEC_MINMAX(N, (U-1)/2, vmin(mSize[l]-1, (U+2)/2)) {
					int n = linIdx(N,l);
					if (mType[l][n] == vtInactive) continue;
							
					// stencil entry at V associated to N (coarse grid level l)
					Vec3i SC = N - V + mStencilMax;
					int sc = SC.x + 3*SC.y + 9*SC.z;
					if (sc < mStencilSize-1) continue;

					// loop over all vertices W which are in the stencil of A_{l-1} at U 
					// and which interpolate from N
					FOR_VEC_MINMAX(W, vmax(           0, vmax(U-1,N*2-1)),
									  vmin(mSize[l-1]-1, vmin(U+1,N*2+1))) {
						int w = linIdx(W,l-1);
						if (mType[l-1][w] == vtInactive) continue;

						// stencil entry at U associated to W (fine grid level l-1)
						Vec3i SF = W - U + mStencilMax;
						int sf = SF.x + 3*SF.y + 9*SF.z;

						Real iw = Real(1) / Real(1 << ((W.x % 2) + (W.y % 2) + (W.z % 2))); // interpolation weight

						if (sf < mStencilSize) {
							mA[l][v*mStencilSize + sc-mStencilSize+1] += rw * mA[l-1][w*mStencilSize + mStencilSize-1-sf] *iw;
						} else {
							mA[l][v*mStencilSize + sc-mStencilSize+1] += rw * mA[l-1][u*mStencilSize + sf-mStencilSize+1] *iw;
						}
					}
				}
			}
		}
	}		
}

void GridMg::smoothGS(int l, bool reversedOrder)
{
	// Multicolor Gauss-Seidel with two colors for the 5/7-point stencil on level 0 
	// and with four/eight colors for the 9/27-point stencil on levels > 0
	std::vector<std::vector<Vec3i>> colorOffs;
	const Vec3i a[8] = {Vec3i(0,0,0), Vec3i(1,0,0), Vec3i(0,1,0), Vec3i(1,1,0), 
		                Vec3i(0,0,1), Vec3i(1,0,1), Vec3i(0,1,1), Vec3i(1,1,1)};
	if (mIs3D) {
		if (l==0) colorOffs = {{a[0],a[3],a[5],a[6]}, {a[1],a[2],a[4],a[7]}};
		else      colorOffs = {{a[0]}, {a[1]}, {a[2]}, {a[3]}, {a[4]}, {a[5]}, {a[6]}, {a[7]}};
	} else {
		if (l==0) colorOffs = {{a[0],a[3]}, {a[1],a[2]}};
		else      colorOffs = {{a[0]}, {a[1]}, {a[2]}, {a[3]}};
	}

	// Divide grid into 2x2 blocks for parallelization
	Vec3i blockSize = (mSize[l]+1)/2;
	int numBlocks = blockSize.x * blockSize.y * blockSize.z;
	
	for (int c = 0; c < colorOffs.size(); c++) {
		int color = reversedOrder ? colorOffs.size()-1-c : c;

		#pragma omp parallel for schedule(static,1)
		for (int b = 0; b < numBlocks; b++)	{
			for (int off = 0; off < colorOffs[color].size(); off++) {
				Vec3i B(b%blockSize.x, (b%(blockSize.x*blockSize.y))/blockSize.x, b/(blockSize.x*blockSize.y));
				
				Vec3i V = 2*B + colorOffs[color][off];
				if (!inGrid(V,l)) continue;
				
				int v = linIdx(V,l);
				if (mType[l][v] == vtInactive) continue;

				Real sum = mb[l][v];

				if (l==0) {
					int n;
					for (int d=0; d<mDim; d++) {
						if (V[d]>0)             { n = v-mPitch[0][d]; sum -= mA[0][n*mStencilSize0 + d+1] * mx[0][n]; }
						if (V[d]<mSize[0][d]-1) { n = v+mPitch[0][d]; sum -= mA[0][v*mStencilSize0 + d+1] * mx[0][n]; }
					}

					mx[0][v] = sum / mA[0][v*mStencilSize0 + 0];
				} else {
					FOR_VECLIN_MINMAX(S, s, mStencilMin, mStencilMax) {
						if (s == mStencilSize-1) continue;

						Vec3i N = V + S;
						int n = linIdx(N,l);

						if (inGrid(N,l) && mType[l][n]!=vtInactive) {
							if (s < mStencilSize) {
								sum -= mA[l][n*mStencilSize + mStencilSize-1-s] * mx[l][n];
							} else {
								sum -= mA[l][v*mStencilSize + s-mStencilSize+1] * mx[l][n];
							}
						}
					}

					mx[l][v] = sum / mA[l][v*mStencilSize + 0];
				}
			}
		}
	}
}

void GridMg::calcResidual(int l)
{
	#pragma omp parallel for schedule(static,1)
	FOR_LVL(v,l) {
		if (mType[l][v] == vtInactive) continue;
		
		Vec3i V = vecIdx(v,l);

		Real sum = mb[l][v];

		if (l==0) {
			int n;
			for (int d=0; d<mDim; d++) {
				if (V[d]>0)             { n = v-mPitch[0][d]; sum -= mA[0][n*mStencilSize0 + d+1] * mx[0][n]; }
				if (V[d]<mSize[0][d]-1) { n = v+mPitch[0][d]; sum -= mA[0][v*mStencilSize0 + d+1] * mx[0][n]; }
			}
			sum -= mA[0][v*mStencilSize0 + 0] * mx[0][v];
		} else {
			FOR_VECLIN_MINMAX(S, s, mStencilMin, mStencilMax) {
				Vec3i N = V + S;
				int n = linIdx(N,l);

				if (inGrid(N,l) && mType[l][n]!=vtInactive) {
					if (s < mStencilSize) {
						sum -= mA[l][n*mStencilSize + mStencilSize-1-s] * mx[l][n];
					} else {
						sum -= mA[l][v*mStencilSize + s-mStencilSize+1] * mx[l][n];
					}
				}
			}
		}

		mr[l][v] = sum;
	}
}

Real GridMg::calcResidualNorm(int l)
{
	Real res = Real(0);

	#pragma omp parallel for reduction(+: res)
	FOR_LVL(v,l) {
		if (mType[l][v] == vtInactive) continue;

		res += mr[l][v] * mr[l][v];
	}

	return std::sqrt(res);
}

// Standard conjugate gradients with Jacobi preconditioner
// Note: not parallelized since coarsest level is assumed to be small
void GridMg::solveCG(int l)
{
	std::vector<Real>& z = mCGtmp1[l];
	std::vector<Real>& p = mCGtmp2[l];

	std::vector<Real>& x = mx[l];
	std::vector<Real>& r = mr[l];

	// Initialization:
	Real alphaTop = Real(0);
	Real initialResidual = Real(0);

	FOR_LVL(v,l) {
		if (mType[l][v] == vtInactive) continue;
		
		Vec3i V = vecIdx(v,l);

		Real sum = mb[l][v];

		if (l==0) {
			int n;
			for (int d=0; d<mDim; d++) {
				if (V[d]>0)             { n = v-mPitch[0][d]; sum -= mA[0][n*mStencilSize0 + d+1] * x[n]; }
				if (V[d]<mSize[0][d]-1) { n = v+mPitch[0][d]; sum -= mA[0][v*mStencilSize0 + d+1] * x[n]; }
			}
			sum -= mA[0][v*mStencilSize0 + 0] * x[v];
			
			z[v] = sum / mA[0][v*mStencilSize0 + 0];
		} else {
			FOR_VECLIN_MINMAX(S, s, mStencilMin, mStencilMax) {
				Vec3i N = V + S;
				int n = linIdx(N,l);

				if (inGrid(N,l) && mType[l][n]!=vtInactive) {
					if (s < mStencilSize) {
						sum -= mA[l][n*mStencilSize + mStencilSize-1-s] * x[n];
					} else {
						sum -= mA[l][v*mStencilSize + s-mStencilSize+1] * x[n];
					}
				}
			}
			
			z[v] = sum / mA[l][v*mStencilSize + 0];
		}

		r[v] = sum;
		initialResidual += r[v] * r[v];
		p[v] = z[v];
		alphaTop += r[v] * z[v];
	}

	initialResidual = std::sqrt(initialResidual);

	int iter = 0;
	const int maxIter = 10000;
	Real residual = Real(-1);

	// CG iterations
	for (; iter<maxIter; iter++)
	{
		Real alphaBot = Real(0);

		FOR_LVL(v,l) {
			if (mType[l][v] == vtInactive) continue;
		
			Vec3i V = vecIdx(v,l);

			z[v] = Real(0);

			if (l==0) {
				int n;
				for (int d=0; d<mDim; d++) {
					if (V[d]>0)             { n = v-mPitch[0][d]; z[v] += mA[0][n*mStencilSize0 + d+1] * p[n]; }
					if (V[d]<mSize[0][d]-1) { n = v+mPitch[0][d]; z[v] += mA[0][v*mStencilSize0 + d+1] * p[n]; }
				}
				z[v] += mA[0][v*mStencilSize0 + 0] * p[v];
			} else {
				FOR_VECLIN_MINMAX(S, s, mStencilMin, mStencilMax) {
					Vec3i N = V + S;
					int n = linIdx(N,l);

					if (inGrid(N,l) && mType[l][n]!=vtInactive) {
						if (s < mStencilSize) {
							z[v] += mA[l][n*mStencilSize + mStencilSize-1-s] * p[n];
						} else {
							z[v] += mA[l][v*mStencilSize + s-mStencilSize+1] * p[n];
						}
					}
				}
			}

			alphaBot += p[v] * z[v];
		}

		Real alpha = alphaTop / alphaBot;
		
		Real alphaTopNew = Real(0);
		residual = Real(0);

		FOR_LVL(v,l) {
			if (mType[l][v] == vtInactive) continue;
		
			x[v] += alpha * p[v];
			r[v] -= alpha * z[v];
			residual += r[v] * r[v];
			if (l==0) z[v] = r[v] / mA[0][v*mStencilSize0 + 0];
			else      z[v] = r[v] / mA[l][v*mStencilSize  + 0];			
			alphaTopNew += r[v] * z[v];
		}

		residual = std::sqrt(residual);

		if (residual / initialResidual < mCoarsestLevelAccuracy) break;

		Real beta = alphaTopNew / alphaTop;
		alphaTop = alphaTopNew;

		FOR_LVL(v,l) {
			p[v] = z[v] + beta * p[v];
		}
	}

	if (iter == maxIter) { debMsg("GridMg::solveCG Warning: Reached maximum number of CG iterations", 1); }
	else { debMsg("GridMg::solveCG Info: Reached residual "<<residual<<" in "<<iter<<" iterations", 2); }
}

void GridMg::restrict(int l_dst, std::vector<Real>& src, std::vector<Real>& dst)
{
	const int l_src = l_dst - 1;
	
	#pragma omp parallel for schedule(static,1)
	FOR_LVL(v,l_dst) {
		if (mType[l_dst][v] == vtInactive) continue;

		// Coarse grid vertex
		Vec3i V = vecIdx(v,l_dst);
		
		Real sum = Real(0);

		FOR_VEC_MINMAX(R, vmax(0, V*2-1), vmin(mSize[l_src]-1, V*2+1)) {
			int r = linIdx(R,l_src);
			if (mType[l_src][r] == vtInactive) continue;

			// restriction weight			
			Real rw = Real(1) / Real(1 << ((R.x % 2) + (R.y % 2) + (R.z % 2))); 
			
			sum += rw * src[r]; 
		}

		dst[v] = sum;
	}
}

void GridMg::interpolate(int l_dst, std::vector<Real>& src, std::vector<Real>& dst)
{
	const int l_src = l_dst + 1;

	#pragma omp parallel for schedule(static,1)
	FOR_LVL(v,l_dst) {
		if (mType[l_dst][v] == vtInactive) continue;
		
		Vec3i V = vecIdx(v,l_dst);

		Real sum = Real(0);

		FOR_VEC_MINMAX(I, V/2, (V+1)/2) {
			int i = linIdx(I,l_src);
			if (mType[l_src][i] != vtInactive) sum += src[i]; 
		}

		// interpolation weight			
		Real iw = Real(1) / Real(1 << ((V.x % 2) + (V.y % 2) + (V.z % 2)));

		dst[v] = iw * sum;
	}
}



}; // DDF
