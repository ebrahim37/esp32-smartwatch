// Obstacle-aware multilayer A*: through-vias only, eight planar directions.
// Binary stdin: nx,ny,nl,ns,ng; blocked[nl*ny*nx], viaOK[ny*nx], starts,goals.
// stdout: signed path length (0 means no legal path), packed state indices.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <queue>
#include <vector>
struct Node { uint32_t id, f, g; };
struct Later { bool operator()(const Node&a,const Node&b)const {return a.f>b.f || (a.f==b.f && a.g<b.g);} };
int main(){
 uint32_t h[5]; if(std::fread(h,4,5,stdin)!=5)return 2;
 const uint32_t nx=h[0],ny=h[1],nl=h[2],ns=h[3],ng=h[4],plane=nx*ny,N=plane*nl;
 if(!nx||!ny||!nl||nl>4||!ns||!ng||N>20000000)return 3;
 std::vector<uint8_t> block(N),via(plane),goal(N,0);
 std::vector<uint32_t> starts(ns),goals(ng),dist(N,UINT32_MAX),prev(N,UINT32_MAX);
 if(std::fread(block.data(),1,N,stdin)!=N || std::fread(via.data(),1,plane,stdin)!=plane || std::fread(starts.data(),4,ns,stdin)!=ns || std::fread(goals.data(),4,ng,stdin)!=ng)return 4;
 for(auto v:goals)if(v<N)goal[v]=1;else return 5;
 auto heuristic=[&](uint32_t u){uint32_t best=UINT32_MAX;const int ux=u%nx,uy=(u%plane)/nx,ul=u/plane;
  for(auto v:goals){const uint32_t dx=std::abs(ux-int(v%nx)),dy=std::abs(uy-int((v%plane)/nx));best=std::min(best,10*std::max(dx,dy)+4*std::min(dx,dy)+(ul!=int(v/plane)?200u:0u));}return best;};
 std::vector<Node> storage;storage.reserve(N);std::priority_queue<Node,std::vector<Node>,Later> q(Later{},std::move(storage));
 for(auto u:starts){if(u<N&&!block[u]){dist[u]=0;q.push({u,heuristic(u),0});}}
 const int dx[8]={1,-1,0,0,1,1,-1,-1},dy[8]={0,0,1,-1,1,-1,1,-1};
 uint32_t finish=UINT32_MAX,expanded=0;
 auto push=[&](uint32_t u,uint32_t v,uint32_t step){const uint32_t g=dist[u]+step;if(!block[v]&&g<dist[v]){dist[v]=g;prev[v]=u;q.push({v,g+heuristic(v),g});}};
 while(!q.empty()){
  Node n=q.top();q.pop();if(n.g!=dist[n.id])continue;uint32_t u=n.id;++expanded;if(goal[u]){finish=u;break;}
  const int x=u%nx,y=(u%plane)/nx,l=u/plane;
  for(int j=0;j<8;++j){int a=x+dx[j],b=y+dy[j];if(a<0||a>=int(nx)||b<0||b>=int(ny))continue;
   if(j>=4&&(block[l*plane+y*nx+a]||block[l*plane+b*nx+x]))continue;push(u,l*plane+b*nx+a,j<4?10:14);}
  if(via[u%plane])for(uint32_t k=0;k<nl;++k)if(k!=uint32_t(l))push(u,k*plane+u%plane,200);
 }
 std::vector<uint32_t> path;
 if(finish!=UINT32_MAX){path.reserve(N);for(uint32_t u=finish;u!=UINT32_MAX;u=prev[u])path.push_back(u);std::reverse(path.begin(),path.end());}
 int32_t count=static_cast<int32_t>(path.size());std::fwrite(&count,4,1,stdout);if(count)std::fwrite(path.data(),4,count,stdout);
 std::fprintf(stderr,"expanded=%u states=%u cost=%u path=%d\n",expanded,N,finish==UINT32_MAX?0:dist[finish],count);
 return 0;
}
