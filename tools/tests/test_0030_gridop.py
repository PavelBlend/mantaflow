#
# Basic test, grid operators
# 

import sys
print ("Running python "+sys.version)

from manta import *
from helperInclude import *


# solver params
gs  = vec3(10, 20, 30)
s   = Solver(name='main', gridSize = gs, dim=3)

# prepare grids
rlg1  = s.create(RealGrid)
rlg2  = s.create(RealGrid)
rlg3  = s.create(RealGrid)
vcg1  = s.create(MACGrid)
vcg2  = s.create(MACGrid)
vcg3  = s.create(MACGrid)
int1  = s.create(IntGrid)
int2  = s.create(IntGrid)
int3  = s.create(IntGrid)

genRefFiles = getGenRefFileSetting() 
if (genRefFiles==1):
	# manually init result
	setConstant    ( rlg1, 1.1 )
	setConstant    ( rlg2, 1.2 )
	setConstant    ( rlg3, 2.9 )

	setConstantVec3( vcg1 , vec3(1.2, 1.2, 1.2) )
	setConstantVec3( vcg2 , vec3(0.5, 0.5, 0.5) )
	setConstantVec3( vcg3 , vec3(1.95, 1.95, 1.95) )

	setConstantInt ( int1 , 125 )
	setConstantInt ( int2 , 6 )
	setConstantInt ( int3 , 143 )
else:	
# real test run, perform basic calculations

	setConstant    ( rlg1, 1.0 )
	setConstant    ( rlg2, 2.4 )
	setConstant    ( rlg3, 9.6 )
	rlg1.addConst (0.1) # 1.1
	rlg2.multConst(0.5)  # 1.2
	rlg3.copyFrom( rlg1 )  # 1.1
	rlg3.add(rlg2)  # 2.3
	rlg3.addScaled(rlg2, 0.5) # 2.9
	#print "r1 %f , r2 %f , r3 %f " % ( rlg1.getMaxAbsValue() , rlg2.getMaxAbsValue() , rlg3.getMaxAbsValue() )

	setConstantVec3( vcg1 , vec3(1.0, 1.0, 1.0) )
	setConstantVec3( vcg2 , vec3(1.0, 1.0, 1.0) )
	setConstantVec3( vcg3 , vec3(9.0, 9.0, 9.0) )
	vcg1.addConst ( vec3(0.2,0.2,0.2) ) # 1.2
	vcg2.multConst( vec3(0.5,0.5,0.5) ) # 0.5
	vcg3.copyFrom( vcg1 )  # 1.2
	vcg3.add(vcg2) # 1.7
	vcg3.addScaled(vcg2, vec3(0.5, 0.5, 0.5) ) # 1.95
	#print "v1 %s , v2 %s , v3 %s " % ( vcg1.getMaxAbsValue() , vcg2.getMaxAbsValue(), vcg3.getMaxAbsValue() )

	setConstantInt ( int1 , 123 )
	setConstantInt ( int2 , 2 )
	setConstantInt ( int3 , 9 )
	int1.addConst ( 2 ) # 125
	int2.multConst( 3 ) # 6
	int3.copyFrom( int1 ) # 125
	int3.add(int2)  # 131
	int3.addScaled(int2, 2) # 143
	#print "i1 %s , i2 %s , i3 %s " % ( int1.getMaxAbsValue() , int2.getMaxAbsValue() , int3.getMaxAbsValue() )


# verify

doTestGrid( sys.argv[0], "rlg1", s, rlg1 , threshold=1e-08 , thresholdStrict=1e-14  )
doTestGrid( sys.argv[0], "rlg2", s, rlg2 , threshold=1e-08 , thresholdStrict=1e-14  )
doTestGrid( sys.argv[0], "rlg3", s, rlg3 , threshold=1e-08 , thresholdStrict=1e-14  )

doTestGrid( sys.argv[0], "vcg1", s, vcg1 , threshold=1e-08 , thresholdStrict=1e-14  )
doTestGrid( sys.argv[0], "vcg2", s, vcg2 , threshold=1e-08 , thresholdStrict=1e-14  )
doTestGrid( sys.argv[0], "vcg3", s, vcg3 , threshold=1e-08 , thresholdStrict=1e-14  )

doTestGrid( sys.argv[0], "int1", s, int1 , threshold=1e-14 , thresholdStrict=1e-14  )
doTestGrid( sys.argv[0], "int2", s, int2 , threshold=1e-14 , thresholdStrict=1e-14  )
doTestGrid( sys.argv[0], "int3", s, int3 , threshold=1e-14 , thresholdStrict=1e-14  )

