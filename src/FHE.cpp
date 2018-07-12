/* Copyright (C) 2012-2017 IBM Corp.
 * This program is Licensed under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance
 * with the License. You may obtain a copy of the License at
 *   http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License. See accompanying LICENSE file.
 */
#include "FHE.h"

#include <queue> // used in the breadth-first search in setKeySwitchMap
#include "timing.h"
#include "binio.h"
#include "sample.h"

NTL_CLIENT

/******** Utility function to generate RLWE instances *********/

// Assumes that c1 is already chosen by the caller
double RLWE1(DoubleCRT& c0, const DoubleCRT& c1, const DoubleCRT &s, long p)
// Returns the variance of the noise canonical-embedding entries
{
  assert (p>0); // Used with p=1 for CKKS, p>=2 for BGV
  const FHEcontext& context = s.getContext();
  const PAlgebra& palg = context.zMStar;

  // choose a short error e
  double stdev = to_double(context.stdev);
  if (palg.getPow2() == 0) // not power of two
    stdev *= sqrt(palg.getM());
  c0.sampleGaussianBounded(stdev);

  // Set c0 =  p*e - c1*s.
  // It is assumed that c0,c1 are defined with respect to the same set of
  // primes, but s may be defined relative to a different set. Either way
  // the primes for of c0,c1 are unchanged.
  if (p>1)
    c0 *= p;
  DoubleCRT tmp(c1);
  tmp.Mul(s, /*matchIndexSets=*/false); // multiply but don't mod-up
  c0 -= tmp;

  return stdev*stdev * p*p *
         ((palg.getPow2()>0)? palg.getM() : palg.getPhiM());
}

// Choose random c0,c1 such that c0+s*c1 = p*e for a short e
// Returns the variance of the noise canonical-embedding entries
double RLWE(DoubleCRT& c0,DoubleCRT& c1, const DoubleCRT &s, long p,
            ZZ* prgSeed)
{
  // choose c1 at random (using prgSeed if not NULL)
  c1.randomize(prgSeed);
  return RLWE1(c0, c1, s, p);
}

/******************** KeySwitch implementation **********************/
/********************************************************************/

bool KeySwitch::operator==(const KeySwitch& other) const
{
  if (this == &other) return true;

  if (fromKey != other.fromKey) return false;
  if (toKeyID != other.toKeyID) return false;
  if (ptxtSpace != other.ptxtSpace) return false;

  if (prgSeed != other.prgSeed) return false;

  if (b.size() != other.b.size()) return false;
  for (size_t i=0; i<b.size(); i++) if (b[i] != other.b[i]) return false;

  return true;
}


void KeySwitch::verify(FHESecKey& sk) 
{
  long fromSPower = fromKey.getPowerOfS();
  long fromXPower = fromKey.getPowerOfX();
  long fromIdx = fromKey.getSecretKeyID(); 
  long toIdx = toKeyID;
  long p = ptxtSpace;
  long n = b.size();

  cout << "KeySwitch::verify\n";
  cout << "fromS = " << fromSPower 
       << " fromX = " << fromXPower 
       << " fromIdx = " << fromIdx 
       << " toIdx = " << toIdx 
       << " p = " << p 
       << " n = " << n 
       << "\n";


  if (fromSPower != 1 || fromXPower != 1 || (fromIdx == toIdx) || n == 0) {
    cout << "KeySwitch::verify: these parameters not checkable\n";
    return;
  }

  const FHEcontext& context = b[0].getContext();

  // we don't store the context in the ks matrix, so let's
  // check that they are consistent

  for (long i = 0; i < n; i++) {
    if (&context != &(b[i].getContext()))
      cout << "KeySwitch::verify: bad context " << i << "\n";
  }

  cout << "context.ctxtPrimes = " << context.ctxtPrimes << "\n";
  cout << "context.specialPrimes = " << context.specialPrimes << "\n";

  IndexSet allPrimes = context.ctxtPrimes | context.specialPrimes;

  cout << "digits: ";
  for (long i = 0; i < n; i++) 
    cout << context.digits[i] << " ";
  cout << "\n";

  cout << "IndexSets of b: ";
  for (long i = 0; i < n; i++) 
    cout << b[i].getMap().getIndexSet() << " ";
  cout << "\n";

  // VJS: suspicious shadowing of fromKey, toKey
  const DoubleCRT& _fromKey = sk.sKeys.at(fromIdx);
  const DoubleCRT& _toKey = sk.sKeys.at(toIdx);

  cout << "IndexSet of fromKey: " << _fromKey.getMap().getIndexSet() << "\n";
  cout << "IndexSet of toKey: " << _toKey.getMap().getIndexSet() << "\n";

  vector<DoubleCRT> a;
  a.resize(n, DoubleCRT(context, allPrimes)); // defined modulo all primes

  { RandomState state;

    SetSeed(prgSeed);
    for (long i = 0; i < n; i++)
      a[i].randomize();

  } // the RandomState destructor "restores the state" (see NumbTh.h)

  vector<ZZX> A, B;

  A.resize(n);
  B.resize(n);

  for (long i = 0; i < n; i++) {
    a[i].toPoly(A[i]);
    b[i].toPoly(B[i]);
  }

  ZZX FromKey, ToKey;
  _fromKey.toPoly(FromKey, allPrimes);
  _toKey.toPoly(ToKey, allPrimes);

  ZZ Q = context.productOfPrimes(allPrimes);
  ZZ prod = context.productOfPrimes(context.specialPrimes);
  ZZX C, D;
  ZZX PhimX = context.zMStar.getPhimX();

  long nb = 0;
  for (long i = 0; i < n; i++) {
    C = (B[i] - FromKey*prod + ToKey*A[i]) % PhimX;
    PolyRed(C, Q);
    if (!divide(D, C, p)) {
      cout << "*** not divisible by p at " << i << "\n";
    }
    else {
      for (long j = 0; j <= deg(D); j++)
         if (NumBits(coeff(D, j)) > nb) nb = NumBits(coeff(D, j));
    }
    prod *= context.productOfPrimes(context.digits[i]);
  }

  cout << "error ratio: " << ((double) nb)/((double) NumBits(Q)) << "\n";
}

const KeySwitch& KeySwitch::dummy()
{
  static const KeySwitch dummy(-1,-1,-1,-1);
  return dummy;
}

ostream& operator<<(ostream& str, const KeySwitch& matrix)
{
  str << "["<<matrix.fromKey  <<" "<<matrix.toKeyID
      << " "<<matrix.ptxtSpace<<" "<<matrix.b.size() << endl;
  for (long i=0; i<(long)matrix.b.size(); i++)
    str << matrix.b[i] << endl;
  str << matrix.prgSeed << " " << matrix.noiseVar << "]";
  return str;
}

// Used in lieu of istream& operator>>(istream& str, KeySwitch& matrix)
void KeySwitch::readMatrix(istream& str, const FHEcontext& context)
{
  //  cerr << "KeySwitch[";
  seekPastChar(str,'['); // defined in NumbTh.cpp
  str >> fromKey;
  str >> toKeyID;
  str >> ptxtSpace;

  long nDigits;
  str >> nDigits;
  b.resize(nDigits, DoubleCRT(context, IndexSet::emptySet()));
  for (long i=0; i<nDigits; i++)
    str >> b[i];
  str >> prgSeed;
  str >> noiseVar;
  seekPastChar(str,']');
  //  cerr << "]";
}


void KeySwitch::write(ostream& str) const
{
  writeEyeCatcher(str, BINIO_EYE_SKM_BEGIN);
/*  
    Write out raw
    1. SKHandle fromKey; 
    2. long     toKeyID;
    3. long     ptxtSpace;
    4. vector<DoubleCRT> b;
    5. ZZ prgSeed;
*/

  fromKey.write(str);
  write_raw_int(str, toKeyID);
  write_raw_int(str, ptxtSpace);

  write_raw_vector(str, b);
  
  write_raw_ZZ(str, prgSeed);
  write_raw_xdouble(str, noiseVar);

  writeEyeCatcher(str, BINIO_EYE_SKM_END);
}

void KeySwitch::read(istream& str, const FHEcontext& context)
{
  assert(readEyeCatcher(str, BINIO_EYE_SKM_BEGIN)==0);

  fromKey.read(str);
  toKeyID = read_raw_int(str);
  ptxtSpace = read_raw_int(str);
  DoubleCRT blankDCRT(context, IndexSet::emptySet());
  read_raw_vector(str, b, blankDCRT);
  read_raw_ZZ(str, prgSeed);
  noiseVar = read_raw_xdouble(str); 

  assert(readEyeCatcher(str, BINIO_EYE_SKM_END)==0);
}



/******************** FHEPubKey implementation **********************/
/********************************************************************/
// Computes the keySwitchMap pointers, using breadth-first search (BFS)

void FHEPubKey::setKeySwitchMap(long keyId)
{
  assert(keyId>=0 && keyId<(long)skSizes.size()); // Sanity-check, do we have such a key?
  long m = context.zMStar.getM();

  // Initialize an aray of "edges" (this is easier than searching through
  // all the matrices for every step). This is a list of all the powers n
  // for which we have a matrix W[s_i(X^n) => s_i(X)], as well as the index
  // of that matrix in the keySwitching array.
  typedef pair<long,long> keySwitchingEdge;
  vector<keySwitchingEdge> edges;
  for (long i=0; i<(long)keySwitching.size(); i++) {
    const KeySwitch& mat = keySwitching.at(i);
    if (mat.toKeyID == keyId && mat.fromKey.getPowerOfS()==1
                             && mat.fromKey.getSecretKeyID()== keyId)
      edges.push_back(keySwitchingEdge(mat.fromKey.getPowerOfX(), i));
  }
  if (keyId>=(long)keySwitchMap.size()) // allocate more space if needed
    keySwitchMap.resize(keyId+1);

  // initialize keySwitchMap[keyId] with m empty entries (with -1 in them)
  keySwitchMap.at(keyId).assign(m,-1);

  // A standard BFS implementation using a FIFO queue (complexity O(V+E))

  std::queue<long> bfsQueue; 
  bfsQueue.push(1);          // Push the target node 1 onto the BFS queue
  while (!bfsQueue.empty()) {
    long currentNode = bfsQueue.front();

    // See what other nodes can reach the current one
    for (long j=0; j<(long)edges.size(); j++) { // go over the edges
      long n = edges[j].first;
      long matrixIndex = edges[j].second;

      long nextNode = MulMod(currentNode, n, m);
      if (keySwitchMap.at(keyId).at(nextNode) == -1) {// A new node: mark it now
	// Record the index of the matrix that we use for the first step
	keySwitchMap[keyId][nextNode] = matrixIndex;

	bfsQueue.push(nextNode);    // push new node onto BFS queue
      }
    }
    bfsQueue.pop();                 // We are done with the current node
  }
}

const KeySwitch& FHEPubKey::getKeySWmatrix(const SKHandle& from, 
					   long toIdx) const
{
  // First try to use the keySwitchMap
  if (from.getPowerOfS()==1 && from.getSecretKeyID()==toIdx 
                            && toIdx < (long)keySwitchMap.size()) {
    long matIdx = keySwitchMap.at(toIdx).at(from.getPowerOfX());
    if (matIdx>=0) { 
      const KeySwitch& matrix = keySwitching.at(matIdx);
      if (matrix.fromKey == from) return matrix;
    }
  }

  // Otherwise resort to linear search
  for (size_t i=0; i<keySwitching.size(); i++) {
    if (keySwitching[i].toKeyID==toIdx && keySwitching[i].fromKey==from)
      return keySwitching[i];
  }
  return KeySwitch::dummy(); // return this if nothing is found
}

const KeySwitch& FHEPubKey::getAnyKeySWmatrix(const SKHandle& from) const
{
  // First try to use the keySwitchMap
  if (from.getPowerOfS()==1 && 
      from.getSecretKeyID() < (long)keySwitchMap.size()) {
    long matIdx = keySwitchMap.at(from.getSecretKeyID()).at(from.getPowerOfX());
    if (matIdx>=0) {
      const KeySwitch& matrix = keySwitching.at(matIdx);
      if (matrix.fromKey == from) return matrix;
    }
  }

  // Otherwise resort to linear search
  for (size_t i=0; i<keySwitching.size(); i++) {
    if (keySwitching[i].fromKey==from) return keySwitching[i];
  }
  return KeySwitch::dummy(); // return this if nothing is found
}


// Encrypts plaintext, result returned in the ciphertext argument. When
// called with highNoise=true, returns a ciphertext with noise level~q/8.
// For BGV, ptxtSpace is the intended plaintext space, which cannot be
//     co-prime with pubEncrKey.ptxtSpace. The returned value is the
//     plaintext-space for the resulting ciphertext, which is their GCD/
// For CKKS, ptxtSpace is a bound on the size of the complex plaintext
//     elements that are encoded in ptxt (before scaling). It is assumed that
//     they are scaled in encoding by context.alMod.encodeScalingFactor().
//     The returned value is the scaling factor in the resulting ciphertexe
//     (which can be larger than the input scaling). The same returned factor
//     is also recorded in ctxt.ratFactor.
long FHEPubKey::Encrypt(Ctxt &ctxt, const ZZX& ptxt, long ptxtSpace,
			bool highNoise) const
{
  FHE_TIMER_START;
  if (getContext().alMod.getTag()==PA_cx_tag)
    return CKKSencrypt(ctxt, ptxt, ptxtSpace);
  // NOTE: Is taking the alMod from the context the right thing to do here?

  assert(this == &ctxt.pubKey);
  cout << "FHEPubKey::Encrypt\n";

  if (ptxtSpace != pubEncrKey.ptxtSpace) { // plaintext-space mistamtch
    ptxtSpace = GCD(ptxtSpace, pubEncrKey.ptxtSpace);
    if (ptxtSpace <= 1) Error("Plaintext-space mismatch on encryption");
  }

  // generate a random encryption of zero from the public encryption key
  ctxt = pubEncrKey;  // already an encryption of zero, just not a random one

  // choose a random small scalar r and a small random error vector e,
  // then set ctxt = r*pubEncrKey + ptstSpace*e + (ptxt,0)
  DoubleCRT e(context, context.ctxtPrimes);
  DoubleCRT r(context, context.ctxtPrimes);
  r.sampleSmall();

  double stdev = to_double(context.stdev);
  if (context.zMStar.getPow2()==0) // not power of two
    stdev *= sqrt(context.zMStar.getM());

  for (size_t i=0; i<ctxt.parts.size(); i++) {  // add noise to all the parts
    ctxt.parts[i] *= r;

    if (highNoise && i == 0) {
      // we sample e so that coefficients are uniform over 
      // [-Q/(8*ptxtSpace)..Q/(8*ptxtSpace)]

      ZZ B;
      B = context.productOfPrimes(context.ctxtPrimes);
      B /= (ptxtSpace*8);
      e.sampleUniform(B);
    }
    else { 
      e.sampleGaussian(stdev);
    }

    e *= ptxtSpace;
    ctxt.parts[i] += e;
  }

  // add in the plaintext
  // FIXME: This relies on the first part, ctxt[0], to have handle to 1
  if (ptxtSpace==2) ctxt.parts[0] += ptxt;

  else { // The general case of ptxtSpace>2: for a ciphertext
         // relative to modulus Q, we add ptxt * Q mod ptxtSpace.
    long QmodP = rem(context.productOfPrimes(ctxt.primeSet), ptxtSpace);
    ctxt.parts[0] += MulMod(ptxt,QmodP,ptxtSpace); // MulMod from module NumbTh
  }

  // fill in the other ciphertext data members
  ctxt.ptxtSpace = ptxtSpace;

  if (highNoise) {
    // hack: we set noiseVar to Q^2/8, which is just below threshold 
    // that will signal an error

    ctxt.noiseVar = xexp(2*context.logOfProduct(context.ctxtPrimes)-log(8.0));
    ctxt.highWaterMark = 0;
  }
  else {
    // We have <skey,ctxt>= r*<skey,pkey> +p*(e0+e1*s) +m,
    // where VAR(<skey,pkey>) is recorded in pubEncrKey.noiseVar,
    //       VAR(r)=phi(m)/2        or m/2
    //       VAR(ei)=sigma^2*phi(m) or sigma^2*m^2
    //                   (both depending on whether m is a power of two),
    //       and VAR(s) depends by the secret-key Hamming size (skSizes).
    // Hence the expected size squared is bounded by:
    // VAR(X)= pubEncrKey.noiseVar*VAR(r) + p^2*(1 + VAR(s)*(VAR(ei)+1))

    double rVar = (context.zMStar.getPow2()==0)?
      (context.zMStar.getPhiM()/2.0) : (context.zMStar.getM()/2.0);
    double eVar = stdev*stdev;
    double sVar = skSizes[0];
    double p2 = ptxtSpace*double(ptxtSpace);
    ctxt.noiseVar = pubEncrKey.noiseVar*rVar + p2*(1+sVar*(eVar+1));
    ctxt.highWaterMark = ctxt.findBaseLevel();
  }
  return ptxtSpace;
}

// FIXME: Some code duplication between here and Encrypt above
long FHEPubKey::CKKSencrypt(Ctxt &ctxt, const ZZX& ptxt, long ptxtSize) const
{
  assert(this == &ctxt.pubKey);

  // generate a random encryption of zero from the public encryption key
  ctxt = pubEncrKey;  // already an encryption of zero, just not a random one

  // choose a random small scalar r and a small random error vector e,
  // then set ctxt = r*pubEncrKey + ptstSpace*e + (ptxt,0)
  DoubleCRT e(context, context.ctxtPrimes);
  DoubleCRT r(context, context.ctxtPrimes);
  r.sampleSmall();

  long m = context.zMStar.getM();
  double stdev = to_double(context.stdev);
  if (context.zMStar.getPow2()==0) // not power of two
    stdev *= sqrt(m);

  for (size_t i=0; i<ctxt.parts.size(); i++) {  // add noise to all the parts
    ctxt.parts[i] *= r;
    e.sampleGaussian(stdev);
    ctxt.parts[i] += e;
  }

  // Compute the noise magnitude, and ensure that the plaintext is
  // scaled up by at least this much
  double rVar = (context.zMStar.getPow2()==0)?
    (context.zMStar.getPhiM()/2.0) : (m/2.0);
  double eVar = stdev*stdev;
  double sVar = skSizes[0];
  double noiseVar = conv<double>(pubEncrKey.noiseVar)*rVar + sVar*(eVar+1);

  long factor = getContext().alMod.getCx().encodeScalingFactor();
  long precision = getContext().alMod.getPPowR();
  long extraFactor = ceil(precision*std::sqrt(noiseVar)*log2(m)/factor);
  if (extraFactor>1) { // scale up some more
    factor *= extraFactor;
    ctxt.parts[0] += ptxt * extraFactor;
    cout << "pkEncrypt: extraFactor="<<extraFactor<<", factor="<<factor<<endl;
  }
  else { // no need for extra scaling
    ctxt.parts[0] += ptxt;
    cout << "pkEncrypt: factor="<<factor<<endl;
  }

  ctxt.noiseVar = noiseVar + rVar*factor*ptxtSize*factor*ptxtSize;
  ctxt.ptxtSpace = 1;
  ctxt.highWaterMark = ctxt.findBaseLevel();
  ctxt.ratFactor = factor;

  return factor;
}

bool FHEPubKey::operator==(const FHEPubKey& other) const
{
  if (this == &other) return true;

  if (&context != &other.context) return false;
  if (!pubEncrKey.equalsTo(other.pubEncrKey, /*comparePkeys=*/false))
    return false;

  if (skSizes.size() != other.skSizes.size()) return false;
  for (size_t i=0; i<skSizes.size(); i++)
    if (skSizes[i] != other.skSizes[i]) return false;

  if (keySwitching.size() != other.keySwitching.size()) return true;
  for (size_t i=0; i<keySwitching.size(); i++)
    if (keySwitching[i] != other.keySwitching[i]) return false;

  if (keySwitchMap.size() != other.keySwitchMap.size()) return false;
  for (size_t i=0; i<keySwitchMap.size(); i++) {
    if (keySwitchMap[i].size() != other.keySwitchMap[i].size()) return false;
    for (size_t j=0; j<keySwitchMap[i].size(); j++)
      if (keySwitchMap[i][j] != other.keySwitchMap[i][j]) return false;
  }

  // compare KS_strategy, ignoring trailing FHE_KSS_UNKNOWN
  long n = KS_strategy.length();
  while (n >= 0 && KS_strategy[n-1] == FHE_KSS_UNKNOWN) n--;
  long n1 = other.KS_strategy.length();
  while (n1 >= 0 && other.KS_strategy[n1-1] == FHE_KSS_UNKNOWN) n1--;
  if (n != n1) return false;
  for (long i: range(n)) {
    if (KS_strategy[i] != other.KS_strategy[i]) return false;
  }

  if (recryptKeyID!=other.recryptKeyID) return false;
  if (recryptKeyID>=0 && 
      !recryptEkey.equalsTo(other.recryptEkey, /*comparePkeys=*/false))
    return false;

  return true;
}


ostream& operator<<(ostream& str, const FHEPubKey& pk)
{
  str << "[";
  writeContextBase(str, pk.getContext());

  // output the public encryption key itself
  str << pk.pubEncrKey << endl;

  // output skSizes in the same format as vec_long
  str << "[";
  for (long i=0; i<(long)pk.skSizes.size(); i++)
    str << pk.skSizes[i]<<" ";
  str << "]\n";

  // output the key-switching matrices
  str << pk.keySwitching.size() << endl;
  for (long i=0; i<(long)pk.keySwitching.size(); i++)
    str << pk.keySwitching[i] << endl;

  // output keySwitchMap in the same format as vec_vec_long
  str << "[";
  for (long i=0; i<(long)pk.keySwitchMap.size(); i++) {
    str << "[";
    for (long j=0; j<(long)pk.keySwitchMap[i].size(); j++)
      str << pk.keySwitchMap[i][j] << " ";
    str << "]\n ";
  }
  str << "]\n";

  str << pk.KS_strategy << "\n";

  // output the bootstrapping key, if any
  str << pk.recryptKeyID << " ";
  if (pk.recryptKeyID>=0) str << pk.recryptEkey << endl;
  return str << "]";
}

istream& operator>>(istream& str, FHEPubKey& pk)
{
  pk.clear();
  //  cerr << "FHEPubKey[";
  seekPastChar(str, '['); // defined in NumbTh.cpp

  // sanity check, verify that basic context parameters are correct
  unsigned long m, p, r;
  vector<long> gens, ords;
  readContextBase(str, m, p, r, gens, ords);
  assert(comparePAlgebra(pk.getContext().zMStar, m, p, r, gens, ords));

  // Get the public encryption key itself
  str >> pk.pubEncrKey;

  // Get the vector of secret-key Hamming-weights
  vec_long vl;
  str >> vl;
  pk.skSizes.resize(vl.length());
  for (long i=0; i<(long)pk.skSizes.size(); i++) pk.skSizes[i] = vl[i];

  // Get the key-switching matrices
  long nMatrices;
  str >> nMatrices;
  pk.keySwitching.resize(nMatrices);
  for (long i=0; i<nMatrices; i++)  // read the matrix from input str
    pk.keySwitching[i].readMatrix(str, pk.getContext());

  // Get the key-switching map
  Vec< Vec<long> > vvl;
  str >> vvl;
  pk.keySwitchMap.resize(vvl.length());
  for (long i=0; i<(long)pk.keySwitchMap.size(); i++) {
    pk.keySwitchMap[i].resize(vvl[i].length());
    for (long j=0; j<(long)pk.keySwitchMap[i].size(); j++)
      pk.keySwitchMap[i][j] = vvl[i][j];
  }

  // build the key-switching map for all keys
  for (long i=pk.skSizes.size()-1; i>=0; i--)
    pk.setKeySwitchMap(i);

  str >> pk.KS_strategy; 

  // Get the bootstrapping key, if any
  str >> pk.recryptKeyID;
  if (pk.recryptKeyID>=0) str >> pk.recryptEkey;

  seekPastChar(str, ']');
  return str;
}
      
void writePubKeyBinary(ostream& str, const FHEPubKey& pk) 
{

  writeEyeCatcher(str, BINIO_EYE_PK_BEGIN);  

// Write out for FHEPubKey
//  1. Context Base 
//  2. Ctxt pubEncrKey;
//  3. vector<long> skSizes;
//  4. vector<KeySwitch> keySwitching;
//  5. vector< vector<long> > keySwitchMap;
//  6. Vec<long> KS_strategy
//  7. long recryptKeyID; 
//  8. Ctxt recryptEkey;

  writeContextBaseBinary(str, pk.getContext());
  pk.pubEncrKey.write(str);
  write_raw_vector(str, pk.skSizes);

  // Keyswitch Matrices
  write_raw_vector(str, pk.keySwitching);

  long sz = pk.keySwitchMap.size();
  write_raw_int(str, sz);
  for(auto v: pk.keySwitchMap)
    write_raw_vector(str, v);

  write_ntl_vec_long(str, pk.KS_strategy); 

  write_raw_int(str, pk.recryptKeyID);
  pk.recryptEkey.write(str);

  writeEyeCatcher(str, BINIO_EYE_PK_END);
}

void readPubKeyBinary(istream& str, FHEPubKey& pk)
{
  assert(readEyeCatcher(str, BINIO_EYE_PK_BEGIN)==0);
 
  //  // TODO code to check context object is what it should be 
  //  // same as the text IO. May be worth putting it in helper func.
  //  std::unique_ptr<FHEcontext> dummy = buildContextFromBinary(str);
  unsigned long m, p, r;
  vector<long> gens, ords;
  readContextBaseBinary(str, m, p, r, gens, ords);
  assert(comparePAlgebra(pk.getContext().zMStar, m, p, r, gens, ords));

  // Read in the rest
  pk.pubEncrKey.read(str);
  read_raw_vector(str, pk.skSizes);

  // Keyswitch Matrices
  read_raw_vector(str, pk.keySwitching, pk.getContext());

  long sz = read_raw_int(str);
  pk.keySwitchMap.clear();
  pk.keySwitchMap.resize(sz);
  for(auto& v: pk.keySwitchMap)
    read_raw_vector(str, v);

  read_ntl_vec_long(str, pk.KS_strategy); 

  pk.recryptKeyID = read_raw_int(str);
  pk.recryptEkey.read(str);

  assert(readEyeCatcher(str, BINIO_EYE_PK_END)==0);
}


/******************** FHESecKey implementation **********************/
/********************************************************************/

bool FHESecKey::operator==(const FHESecKey& other) const
{
  if (this == &other) return true;

  if (((const FHEPubKey&)*this)!=((const FHEPubKey&)other)) return false;
  if (sKeys.size() != other.sKeys.size()) return false;
  for (size_t i=0; i<sKeys.size(); i++)
    if (sKeys[i] != other.sKeys[i]) return false;
  return true;
}

// We allow the calling application to choose a secret-key polynomial by
// itself, then insert it into the FHESecKey object, getting the index of
// that secret key in the sKeys list. If this is the first secret-key for this
// FHESecKey object, then the procedure below generates a corresponding public
// encryption key.
// It is assumed that the context already contains all parameters.
long FHESecKey::ImportSecKey(const DoubleCRT& sKey, long size,
			     long ptxtSpace, long maxDegKswitch)
{
  bool ckks = (getContext().alMod.getTag()==PA_cx_tag);
  
  if (sKeys.empty()) { // 1st secret-key, generate corresponding public key
    if (ptxtSpace<2)
      ptxtSpace = ckks? 1 : context.alMod.getPPowR();
    // default plaintext space is p^r for BGV, 1 for CKKS

    // allocate space
    pubEncrKey.parts.assign(2,CtxtPart(context,context.ctxtPrimes));
    // Choose a new RLWE instance
    pubEncrKey.noiseVar
      = RLWE(pubEncrKey.parts[0], pubEncrKey.parts[1], sKey, ptxtSpace);
    if (ckks)
      pubEncrKey.ratFactor = sqrt(pubEncrKey.noiseVar);

    // make parts[0],parts[1] point to (1,s)
    pubEncrKey.parts[0].skHandle.setOne();
    pubEncrKey.parts[1].skHandle.setBase();

    // Set the other Ctxt bookeeping parameters in pubEncrKey
    pubEncrKey.primeSet = context.ctxtPrimes;
    pubEncrKey.ptxtSpace = ptxtSpace;
  }
  skSizes.push_back(size); // record the size of the new secret-key
  sKeys.push_back(sKey); // add to the list of secret keys
  long keyID = sKeys.size()-1; // not thread-safe?

  for (long e=2; e<=maxDegKswitch; e++)
    GenKeySWmatrix(e,1,keyID,keyID); // s^e -> s matrix

  if (keyID==0)
    pubEncrKey.highWaterMark = pubEncrKey.findBaseLevel();

  return keyID; // return the index where this key is stored
}

// Generate a key-switching matrix and store it in the public key.
// The argument p denotes the plaintext space
void FHESecKey::GenKeySWmatrix(long fromSPower, long fromXPower,
			       long fromIdx, long toIdx, long p)
{
  FHE_TIMER_START;

  // sanity checks
  if (fromSPower<=0 || fromXPower<=0) return;  
  if (fromSPower==1 && fromXPower==1 && fromIdx==toIdx) return;

  // See if this key-switching matrix already exists in our list
  if (haveKeySWmatrix(fromSPower, fromXPower, fromIdx, toIdx))
    return; // nothing to do here

  DoubleCRT fromKey = sKeys.at(fromIdx); // copy object, not a reference
  const DoubleCRT& toKey = sKeys.at(toIdx);   // this can be a reference

  if (fromXPower>1) fromKey.automorph(fromXPower); // compute s(X^t)
  if (fromSPower>1) fromKey.Exp(fromSPower);       // compute s^r(X^t)
  // SHAI: The above lines compute the automorphism and exponentiation mod q,
  //   turns out this is really what we want (even through usually we think
  //   of the secret key as being mod p^r)

  KeySwitch ksMatrix(fromSPower,fromXPower,fromIdx,toIdx);
  RandomBits(ksMatrix.prgSeed, 256); // a random 256-bit seed

  long n = context.digits.size();

  ksMatrix.b.resize(n, DoubleCRT(context)); // size-n vector

  vector<DoubleCRT> a; 
  a.resize(n, DoubleCRT(context));

  { RandomState state;
    SetSeed(ksMatrix.prgSeed);
    for (long i = 0; i < n; i++) 
      a[i].randomize();
  } // restore state upon destruction of state

  // Record the plaintext space for this key-switching matrix
  if (getContext().alMod.getTag()==PA_cx_tag) // CKKS
    p = 1;
  else {                                      // BGV
    if (p<2) {
      if (context.isBootstrappable()) // use larger bootstrapping plaintext space
        p = context.rcData.alMod->getPPowR();
      else p = pubEncrKey.ptxtSpace; // default plaintext space from public key
    }
    // FIXME: We use context.isBootstrappable() rather than
    //   this->isBootstrappable(). So we get the larger bootstrapping
    //   plaintext space even if *this is not currently bootstrapppable,
    //   in case the calling application will make it bootstrappable later.

    assert(p>=2);
  }
  ksMatrix.ptxtSpace = p;

  // generate the RLWE instances with pseudorandom ai's

  for (long i = 0; i < n; i++) {
    ksMatrix.noiseVar = RLWE1(ksMatrix.b[i], a[i], toKey, p);
  }
  // Add in the multiples of the fromKey secret key
  fromKey *= context.productOfPrimes(context.specialPrimes);
  for (long i = 0; i < n; i++) {
    ksMatrix.b[i] += fromKey;
    fromKey *= context.productOfPrimes(context.digits[i]);
  }

  // Push the new matrix onto our list
  keySwitching.push_back(ksMatrix);
}

// Decryption
void FHESecKey::Decrypt(ZZX& plaintxt, const Ctxt &ciphertxt) const
{
  ZZX f;
  Decrypt(plaintxt, ciphertxt, f);
}
void FHESecKey::Decrypt(ZZX& plaintxt, const Ctxt &ciphertxt,
			ZZX& f) const // plaintext before modular reduction
{
  FHE_TIMER_START;
#ifdef DEBUG_PRINTOUT
  // The call to findBaseSet is only for the purpose of printing a
  // warning if the noise is large enough so as to risk decryption error
  IndexSet s; ciphertxt.findBaseSet(s);
#endif
  assert(getContext()==ciphertxt.getContext());
  const IndexSet& ptxtPrimes = ciphertxt.primeSet;
  DoubleCRT ptxt(context, ptxtPrimes); // Set to zero

  // for each ciphertext part, fetch the right key, multiply and add
  for (size_t i=0; i<ciphertxt.parts.size(); i++) {
    const CtxtPart& part = ciphertxt.parts[i];
    if (part.skHandle.isOne()) { // No need to multiply
      ptxt += part;
      continue;
    }

    long keyIdx = part.skHandle.getSecretKeyID();
    DoubleCRT key = sKeys.at(keyIdx); // copy object, not a reference
    const IndexSet extraPrimes = key.getIndexSet() / ptxtPrimes;
    key.removePrimes(extraPrimes);    // drop extra primes, for efficiency

    /* Perhaps a slightly more efficient way of doing the same thing is:
       DoubleCRT key(context, ptxtPrimes); // a zero object wrt ptxtPrimes
       key.Add(sKeys.at(keyIdx), false); // add without mathcing primesSet
    */
    long xPower = part.skHandle.getPowerOfX();
    long sPower = part.skHandle.getPowerOfS();
    if (xPower>1) { 
      key.automorph(xPower); // s(X^t)
    }
    if (sPower>1) {
      key.Exp(sPower);       // s^r(X^t)
    }
    key *= part;
    ptxt += key;
  }
  // convert to coefficient representation & reduce modulo the plaintext space
  ptxt.toPoly(plaintxt);
  f = plaintxt; // f used only for debugging

  // FIXME: handle intFactor

  if (ciphertxt.getPtxtSpace() == 1) // CKKS encryption
    return;

  if (ciphertxt.getPtxtSpace()>2) { // if p>2, multiply by Q^{-1} mod p
    long qModP = rem(context.productOfPrimes(ciphertxt.getPrimeSet()), 
                     ciphertxt.ptxtSpace);
    if (qModP != 1) {
      qModP = InvMod(qModP, ciphertxt.ptxtSpace);
      MulMod(plaintxt, plaintxt, qModP, ciphertxt.ptxtSpace);
    }
  }
  PolyRed(plaintxt, ciphertxt.ptxtSpace, true/*reduce to [0,p-1]*/);
}

// Encryption using the secret key, this is useful, e.g., to put an
// encryption of the secret key into the public key.
long FHESecKey::skEncrypt(Ctxt &ctxt, const ZZX& ptxt,
                          long ptxtSpace, long skIdx) const
{
  FHE_TIMER_START;

  bool ckks = (getContext().alMod.getTag()==PA_cx_tag);
  // NOTE: Is taking the alMod from the context the right thing to do here?

  assert(((FHEPubKey*)this) == &ctxt.pubKey);

  long m = getContext().zMStar.getM();
  long ptxtSize = 0;
  if (ckks) {
    ptxtSize = ptxtSpace;
    ptxtSpace = 1;
  }
  else { // BGV
    if (ptxtSpace<2) 
      ptxtSpace = pubEncrKey.ptxtSpace; // default plaintext space is p^r
    assert(ptxtSpace >= 2);
  }
  ctxt.ptxtSpace = ptxtSpace;

  ctxt.primeSet = context.ctxtPrimes; // initialize the primeSet
  {CtxtPart tmpPart(context, context.ctxtPrimes);
  ctxt.parts.assign(2,tmpPart);}      // allocate space

  // Set Ctxt bookeeping parameters

  // make parts[0],parts[1] point to (1,s)
  ctxt.parts[0].skHandle.setOne();
  ctxt.parts[1].skHandle.setBase(skIdx);

  const DoubleCRT& sKey = sKeys.at(skIdx);   // get key
  // Sample a new RLWE instance
  double noiseVar = RLWE(ctxt.parts[0], ctxt.parts[1], sKey, ptxtSpace);

  if (ckks) {
    long factor = getContext().alMod.getCx().encodeScalingFactor();
    long precision = getContext().alMod.getPPowR();
    long extraFactor = ceil(precision*std::sqrt(noiseVar)*log2(m)/factor);
    if (extraFactor>1) { // scale up some more
      factor *= extraFactor;
      ctxt.parts[0] += extraFactor * ptxt;
      cout << "skEncrypt: extraFactor="<<extraFactor<<", factor="<<factor<<endl;
    }
    else {
      ctxt.parts[0] += ptxt;
      cout << "skEncrypt: factor="<<factor<<endl;
    }
    ctxt.ratFactor = factor;
    double rVar = (getContext().zMStar.getPow2()==0)?
      (getContext().zMStar.getPhiM()/4.0) : (m/4.0);
    ctxt.noiseVar = noiseVar + rVar*factor*ptxtSize*factor*ptxtSize;
    ctxt.highWaterMark = ctxt.findBaseLevel();
    return factor;
  }
  else { // BGV
    ctxt.noiseVar = noiseVar;
    ctxt.highWaterMark = ctxt.findBaseLevel();
    ctxt.addConstant(ptxt);  // add in the plaintext
    return ctxt.ptxtSpace;
  }
}


// Generate bootstrapping data if needed, returns index of key
long FHESecKey::genRecryptData()
{
  if (recryptKeyID>=0) return recryptKeyID;

  // Make sure that the context has the bootstrapping EA and PAlgMod
  assert(context.isBootstrappable());

  long p2ePr = context.rcData.alMod->getPPowR();// p^{e-e'+r}
  long p2r = context.alMod.getPPowR(); // p^r

  // Generate a new bootstrapping key
  zzX keyPoly;
  const long hwt = context.rcData.skHwt;
  sampleHWt(keyPoly, context.zMStar, hwt);
  DoubleCRT newSk(keyPoly, context); // defined relative to all primes
  long keyID = ImportSecKey(newSk, hwt, p2r, /*maxDegKswitch=*/1);

  // Generate a key-switching matrix from key 0 to this key
  GenKeySWmatrix(/*fromSPower=*/1,/*fromXPower=*/1,
		 /*fromIdx=*/0,   /*toIdx=*/keyID, /*ptxtSpace=*/p2r);

  // Encrypt new key under key #0 and plaintext space p^{e+r}
  Encrypt(recryptEkey, keyPoly, p2ePr);

  return (recryptKeyID=keyID); // return the new key-ID
}


ostream& operator<<(ostream& str, const FHESecKey& sk)
{
  str << "[" << ((const FHEPubKey&)sk) << endl
      << sk.sKeys.size() << endl;
  for (long i=0; i<(long)sk.sKeys.size(); i++)
    str << sk.sKeys[i] << endl;
  return str << "]";
}

istream& operator>>(istream& str, FHESecKey& sk)
{
  sk.clear();
  //  cerr << "FHESecKey[";
  seekPastChar(str, '['); // defined in NumbTh.cpp
  str >> (FHEPubKey&) sk;

  long nKeys;
  str >> nKeys;
  sk.sKeys.resize(nKeys, DoubleCRT(sk.getContext(),IndexSet::emptySet()));
  for (long i=0; i<nKeys; i++) str >> sk.sKeys[i];
  seekPastChar(str, ']');
  //  cerr << "]\n";
  return str;
}


void writeSecKeyBinary(ostream& str, const FHESecKey& sk)
{
  writeEyeCatcher(str, BINIO_EYE_SK_BEGIN);

  // Write out the public key part first.
  writePubKeyBinary(str, sk);

// Write out 
// 1. vector<DoubleCRT> sKeys  

  write_raw_vector<DoubleCRT>(str, sk.sKeys); 

  writeEyeCatcher(str, BINIO_EYE_SK_END);
}

void readSecKeyBinary(istream& str, FHESecKey& sk)
{
  assert(readEyeCatcher(str, BINIO_EYE_SK_BEGIN)==0);

  // Read in the public key part first.
  readPubKeyBinary(str, sk);

  DoubleCRT blankDCRT(sk.getContext(), IndexSet::emptySet());
  read_raw_vector<DoubleCRT>(str, sk.sKeys, blankDCRT);

  assert(readEyeCatcher(str, BINIO_EYE_SK_END)==0);
}

