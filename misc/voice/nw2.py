import pickle,numpy as np,json,sys
A=pickle.load(open('align_cost.pkl','rb'));B=pickle.load(open('align_cost2.pkl','rb'))
def z(C): return (C-C.mean())/C.std()
def nw(S,gap):
    n,m=S.shape;F=np.zeros((n+1,m+1));T=np.zeros((n+1,m+1),int)
    F[1:,0]=gap*np.arange(1,n+1);F[0,1:]=gap*np.arange(1,m+1);T[1:,0]=1;T[0,1:]=2
    for i in range(1,n+1):
        for j in range(1,m+1):
            c=(F[i-1,j-1]+S[i-1,j-1],F[i-1,j]+gap,F[i,j-1]+gap);k=int(np.argmax(c));F[i,j]=c[k];T[i,j]=k
    i,j=n,m;pr={}
    while i>0 or j>0:
        k=T[i,j]
        if i>0 and j>0 and k==0: pr[i-1]=j-1;i-=1;j-=1
        elif i>0 and (k==1 or j==0): i-=1
        else: j-=1
    return pr
res={}
for name in ('main','A'):
    mk,lk,C1=A[name];_,_,C2=B[name]
    Z1,Z2=z(C1),z(C2)
    a1=nw(-Z1,-0.6);a2=nw(-Z2,-0.6);a3=nw(-(Z1+Z2)/1.4,-0.6)
    agree=sum(1 for i in a3 if a1.get(i)==a3[i] and a2.get(i)==a3[i])
    print(name,'pairs',len(a3),'agree all three',agree,'mfcc==pros',sum(1 for i in a1 if a2.get(i)==a1[i]))
    # independent per-row argmin agreement with alignment
    am=np.argmin(Z1+Z2,1);print(' rows whose best single match equals aligned',sum(1 for i in a3 if am[i]==a3[i]))
    res[name]={hex(mk[i]):(hex(lk[j]),bool(a1.get(i)==j and a2.get(i)==j)) for i,j in a3.items()}
json.dump(res,open('align2.json','w'),indent=0)
