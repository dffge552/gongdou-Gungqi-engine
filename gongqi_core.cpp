/*
 * gongqi_core.cpp  ─  宮棋 V1 MCTS 核心（WASM 版）
 *
 * 編譯（用 clang wasm32-wasi）:
 *   clang++ --target=wasm32 -O2 -std=c++17 -nostdlib++ ...
 *
 * 匯出函式：
 *   void  init()
 *   void  set_cell(int r, int c, int v)   // v: 0=空 1=黑 -1=白
 *   int   get_cell(int r, int c)
 *   void  clear_board()
 *   int   best_move_row(int player, double time_ms)
 *   int   best_move_col(int player, double time_ms)
 *   int   last_iters()
 */

#include <cstdint>
#include <cmath>
#include <cstring>
#include <initializer_list>

// ── 亂數（xoshiro128+，不依賴 <random>）────────────────────────────
static uint32_t rng_s[4] = {0xdeadbeef,0x12345678,0xabcdef01,0x87654321};
static inline uint32_t rotl(uint32_t x,int k){return (x<<k)|(x>>(32-k));}
static uint32_t rng_next(){
    uint32_t r=rng_s[0]+rng_s[3];
    uint32_t t=rng_s[1]<<9;
    rng_s[2]^=rng_s[0]; rng_s[3]^=rng_s[1];
    rng_s[1]^=rng_s[2]; rng_s[0]^=rng_s[3];
    rng_s[2]^=t; rng_s[3]=rotl(rng_s[3],11);
    return r;
}
static void rng_seed(uint32_t s){rng_s[0]=s;rng_s[1]=s^0x9e3779b9;rng_s[2]=s*2654435761u;rng_s[3]=s^0x517cc1b727220a95u;}

// ── 基本型別 ────────────────────────────────────────────────────────
static const int N     = 10;
static const int EMPTY =  0;
static const int BLACK =  1;
static const int WHITE = -1;
static const int MAX_CELLS = N*N;

struct Pos{ int8_t r,c; };
struct Line{ Pos cells[N]; int8_t len; };

// ── 預計算的線段 ─────────────────────────────────────────────────────
static Line  ALL_LINES[100];
static int   LINE_COUNT = 0;
static int8_t CELL_LINES[N][N][50];
static int8_t CELL_LINE_CNT[N][N];

static void add_line(Pos* pts, int len){
    if(len < 3) return;
    int idx = LINE_COUNT++;
    ALL_LINES[idx].len = len;
    for(int i=0;i<len;i++) ALL_LINES[idx].cells[i]=pts[i];
    for(int i=0;i<len;i++){
        int r=pts[i].r, c=pts[i].c;
        int k=CELL_LINE_CNT[r][c]++;
        CELL_LINES[r][c][k]=idx;
    }
}

static void build_lines(){
    LINE_COUNT=0;
    memset(CELL_LINE_CNT,0,sizeof(CELL_LINE_CNT));
    Pos tmp[N];
    // 橫列
    for(int r=0;r<N;r++){for(int c=0;c<N;c++)tmp[c]={int8_t(r),int8_t(c)};add_line(tmp,N);}
    // 直行
    for(int c=0;c<N;c++){for(int r=0;r<N;r++)tmp[r]={int8_t(r),int8_t(c)};add_line(tmp,N);}
    // 對角線（左上→右下）
    for(int s=-(N-1);s<N;s++){
        int len=0;
        for(int i=0;i<N;i++){int r=s+i,c=i;if(r>=0&&r<N)tmp[len++]={int8_t(r),int8_t(c)};}
        add_line(tmp,len);
    }
    // 對角線（右上→左下）
    for(int s=0;s<2*N-1;s++){
        int len=0;
        for(int i=0;i<N;i++){int r=s-i,c=i;if(r>=0&&r<N)tmp[len++]={int8_t(r),int8_t(c)};}
        add_line(tmp,len);
    }
}

// ── 棋盤 ─────────────────────────────────────────────────────────────
typedef int8_t Board[N][N];

static Board g_board;

// ── 小工具 ──────────────────────────────────────────────────────────
static inline void board_copy(Board dst, const Board src){
    memcpy(dst,src,sizeof(Board));
}

// ── Group 搜尋 ───────────────────────────────────────────────────────
struct Group{ int start,len; Pos pos[N]; int plen; };

static int find_groups(const Line& line,const int8_t board[][N],int player,Group* out){
    int cnt=0;
    // 收集同色石
    int idx[N]; int idxn=0;
    for(int i=0;i<line.len;i++)
        if(board[line.cells[i].r][line.cells[i].c]==player)
            idx[idxn++]=i;
    if(idxn==0) return 0;
    int s=0;
    for(int k=1;k<=idxn;k++){
        if(k==idxn||idx[k]!=idx[k-1]+1){
            int len=k-s;
            if(len>=2){
                Group& g=out[cnt++];
                g.start=idx[s]; g.len=len; g.plen=0;
                for(int j=s;j<k;j++) g.pos[g.plen++]=line.cells[idx[j]];
            }
            s=k;
        }
    }
    return cnt;
}

// ── 平行吃子 ─────────────────────────────────────────────────────────
static int find_parallel(const Group& g,const int8_t board[][N],int opp,bool both,Pos* out){
    int dr=g.plen>=2?(g.pos[1].r-g.pos[0].r):0;
    int dc=g.plen>=2?(g.pos[1].c-g.pos[0].c):0;
    int perp[2][2];
    if(dr==0){perp[0][0]=-1;perp[0][1]=0;perp[1][0]=1;perp[1][1]=0;}
    else if(dc==0){perp[0][0]=0;perp[0][1]=-1;perp[1][0]=0;perp[1][1]=1;}
    else{perp[0][0]=-dr;perp[0][1]=dc;perp[1][0]=dr;perp[1][1]=-dc;}
    int total=0;
    for(int d=0;d<2;d++){
        int pdr=perp[d][0],pdc=perp[d][1];
        bool valid=true; Pos cands[N]; int nc=0;
        for(int i=0;i<g.plen;i++){
            int nr=g.pos[i].r+pdr,ncc=g.pos[i].c+pdc;
            if(nr<0||nr>=N||ncc<0||ncc>=N||board[nr][ncc]!=opp){valid=false;break;}
            cands[nc++]={int8_t(nr),int8_t(ncc)};
        }
        if(valid&&nc==g.plen){
            for(int i=0;i<nc;i++) out[total++]=cands[i];
            if(!both) break;
        }
    }
    return total;
}

// ── 保護判斷 ─────────────────────────────────────────────────────────
static bool is_protected(const int8_t board[][N], Pos pos){
    int player=board[pos.r][pos.c];
    if(player==EMPTY) return false;
    for(int li=0;li<CELL_LINE_CNT[pos.r][pos.c];li++){
        int idx=CELL_LINES[pos.r][pos.c][li];
        Group grps[N/2]; int gc=find_groups(ALL_LINES[idx],board,player,grps);
        for(int gi=0;gi<gc;gi++){
            if(grps[gi].len<3) continue;
            for(int pi=0;pi<grps[gi].plen;pi++)
                if(grps[gi].pos[pi].r==pos.r&&grps[gi].pos[pi].c==pos.c)
                    return true;
        }
    }
    return false;
}

// ── 吃子計算 ─────────────────────────────────────────────────────────
struct Captures{int black,white;};

static Captures calculate_captures(int8_t board[][N]){
    Captures result={0,0};
    static const int priority[]={6,5,4,3};
    for(int pi=0;pi<4;pi++){
        int pr=priority[pi];
        // 收集所有 action
        struct Action{int player;Pos pos[12];int plen;};
        static Action actions[200]; int an=0;
        for(int li=0;li<LINE_COUNT;li++){
            for(int pl:{BLACK,WHITE}){
                Group grps[N/2]; int gc=find_groups(ALL_LINES[li],board,pl,grps);
                for(int gi=0;gi<gc;gi++){
                    Group& g=grps[gi];
                    if(g.len!=pr) continue;
                    int opp=-pl;
                    Action act; act.player=pl; act.plen=0;
                    if(pr==3){
                        if(g.start>0){Pos lp=ALL_LINES[li].cells[g.start-1];if(board[lp.r][lp.c]==opp)act.pos[act.plen++]=lp;}
                        int ri=g.start+g.len;if(ri<ALL_LINES[li].len){Pos rp=ALL_LINES[li].cells[ri];if(board[rp.r][rp.c]==opp)act.pos[act.plen++]=rp;}
                        if(act.plen>0) act.plen=1;
                    } else if(pr==4){
                        if(g.start>0){Pos lp=ALL_LINES[li].cells[g.start-1];if(board[lp.r][lp.c]==opp)act.pos[act.plen++]=lp;}
                        int ri=g.start+g.len;if(ri<ALL_LINES[li].len){Pos rp=ALL_LINES[li].cells[ri];if(board[rp.r][rp.c]==opp)act.pos[act.plen++]=rp;}
                    } else if(pr==5){
                        act.plen=find_parallel(g,board,opp,false,act.pos);
                    } else {
                        act.plen=find_parallel(g,board,opp,true,act.pos);
                    }
                    if(act.plen>0&&an<200) actions[an++]=act;
                }
            }
        }
        for(int ai=0;ai<an;ai++){
            int pl=actions[ai].player;
            for(int pi2=0;pi2<actions[ai].plen;pi2++){
                Pos pos=actions[ai].pos[pi2];
                if(board[pos.r][pos.c]!=-pl) continue;
                if(is_protected(board,pos)) continue;
                board[pos.r][pos.c]=EMPTY;
                if(pl==BLACK) result.black++; else result.white++;
            }
        }
    }
    return result;
}

// ── Rollout ──────────────────────────────────────────────────────────
static float rollout_v1(int8_t board[][N], int player){
    Pos empty[MAX_CELLS]; int en=0;
    for(int r=0;r<N;r++) for(int c=0;c<N;c++)
        if(board[r][c]==EMPTY) empty[en++]={int8_t(r),int8_t(c)};
    // Fisher-Yates shuffle
    for(int i=en-1;i>0;i--){int j=rng_next()%(i+1);Pos t=empty[i];empty[i]=empty[j];empty[j]=t;}
    int bp=0,wp=0;
    for(int r=0;r<N;r++) for(int c=0;c<N;c++){if(board[r][c]==BLACK)bp++;if(board[r][c]==WHITE)wp++;}
    int br=50-bp, wr=50-wp;
    int cur=player;
    for(int i=0;i<en;i++){
        auto [r,c]=empty[i];
        if(cur==BLACK&&br>0){board[r][c]=BLACK;br--;}
        else if(cur==WHITE&&wr>0){board[r][c]=WHITE;wr--;}
        else if(br>0){board[r][c]=BLACK;br--;}
        else{board[r][c]=WHITE;wr--;}
        cur=-cur;
    }
    Captures cap=calculate_captures(board);    float diff=(player==BLACK)?(float)(cap.black-cap.white):(float)(cap.white-cap.black);
    return diff/50.f;
}

// ── MCTS Node Pool ───────────────────────────────────────────────────
static const int MAX_NODES = 1<<19; // 512K nodes
struct Node{
    int8_t mr,mc;     // move
    int8_t player;
    int parent;
    int children[MAX_CELLS]; int8_t nchild;
    Pos untried[MAX_CELLS];  int8_t nuntried;
    int visits;
    float value, rval;
    int rv;
};
static Node pool[MAX_NODES];
static int  pool_size;

static int node_new(int8_t mr,int8_t mc,int8_t pl,int par,Pos* un,int un_n){
    if(pool_size>=MAX_NODES) return -1;
    int idx=pool_size++;
    Node& nd=pool[idx];
    nd.mr=mr; nd.mc=mc; nd.player=pl; nd.parent=par;
    nd.nchild=0; nd.nuntried=(int8_t)un_n;
    for(int i=0;i<un_n;i++) nd.untried[i]=un[i];
    nd.visits=0; nd.value=0; nd.rval=0; nd.rv=0;
    return idx;
}

static float node_score(const Node& nd,float c,int pv,int k=300){
    if(nd.visits==0) return 1e9f;
    float ex=nd.value/nd.visits;
    if(nd.rv>0){
        float rave=nd.rval/nd.rv;
        float beta=sqrtf((float)k/(3.f*nd.visits+k));
        ex=(1-beta)*ex+beta*rave;
    }
    return ex+c*sqrtf(logf((float)(pv<1?1:pv))/(float)nd.visits);
}

// ── 全域結果 ─────────────────────────────────────────────────────────
static int g_result_row=-1, g_result_col=-1, g_result_iters=0;

// ── 主要 MCTS 函式 ────────────────────────────────────────────────────
static void uct_move(int player, int time_ms){
    Pos empty[MAX_CELLS]; int en=0;
    for(int r=0;r<N;r++) for(int c=0;c<N;c++)
        if(g_board[r][c]==EMPTY) empty[en++]={int8_t(r),int8_t(c)};
    if(en==0){g_result_row=-1;g_result_col=-1;g_result_iters=0;return;}
    if(en==1){g_result_row=empty[0].r;g_result_col=empty[0].c;g_result_iters=1;return;}

    pool_size=0;
    // shuffle untried
    for(int i=en-1;i>0;i--){int j=rng_next()%(i+1);Pos t=empty[i];empty[i]=empty[j];empty[j]=t;}
    int root=node_new(-1,-1,(int8_t)(-player),-1,empty,en);

    // 時間控制（簡易：用迭代數估算）
    // 因為 WASM 裡用不了 steady_clock，改用外部傳入的 time_ms 換算迭代次數
    // 粗估：每次 rollout 約 10us → time_ms * 100 次
    int max_iters = time_ms * 80;
    if(max_iters < 100) max_iters=100;
    if(max_iters > 200000) max_iters=200000;

    float c=1.0f;
    for(int iter=0;iter<max_iters;iter++){
        int ni=root;
        Board sim; board_copy(sim,g_board);
        int cur=player;

        // Selection
        while(pool[ni].nuntried==0&&pool[ni].nchild>0){
            int bc=-1; float bs=-1e9f;
            int pv=pool[ni].visits<1?1:pool[ni].visits;
            for(int ci=0;ci<pool[ni].nchild;ci++){
                int ch=pool[ni].children[ci];
                float s=node_score(pool[ch],c,pv);
                if(s>bs){bs=s;bc=ch;}
            }
            ni=bc;
            if(pool[ni].mr>=0) sim[pool[ni].mr][pool[ni].mc]=cur;
            cur=-cur;
        }

        // Expansion
        if(pool[ni].nuntried>0){
            int pick=rng_next()%pool[ni].nuntried;
            Pos mv=pool[ni].untried[pick];
            // 移除 pick
            pool[ni].nuntried--;
            pool[ni].untried[pick]=pool[ni].untried[pool[ni].nuntried];
            sim[mv.r][mv.c]=cur;
            // 新節點的 untried
            Pos un2[MAX_CELLS]; int un2n=0;
            for(int r=0;r<N;r++) for(int cc=0;cc<N;cc++)
                if(sim[r][cc]==EMPTY) un2[un2n++]={int8_t(r),int8_t(cc)};
            for(int i=un2n-1;i>0;i--){int j=rng_next()%(i+1);Pos t=un2[i];un2[i]=un2[j];un2[j]=t;}
            int ci=node_new(mv.r,mv.c,(int8_t)(-cur),ni,un2,un2n);
            if(ci<0) break; // pool full
            pool[ni].children[pool[ni].nchild++]=ci;
            ni=ci; cur=-cur;
        }

        // Rollout
        Board sim_copy; board_copy(sim_copy, sim);
        float reward=rollout_v1(sim_copy,cur);
        if(cur!=player) reward=-reward;

        // Backprop
        for(int n=ni;n!=-1;n=pool[n].parent){
            pool[n].visits++;
            if(pool[n].parent!=-1){
                int mover=-pool[n].player;
                float r2=(mover==player)?reward:-reward;
                pool[n].value+=r2;
                pool[n].rv++; pool[n].rval+=r2;
            }
        }
    }

    // 選最多 visits 的子節點
    int bci=-1,bv=-1;
    for(int ci=0;ci<pool[root].nchild;ci++){
        int ch=pool[root].children[ci];
        if(pool[ch].visits>bv){bv=pool[ch].visits;bci=ch;}
    }
    if(bci<0){g_result_row=empty[0].r;g_result_col=empty[0].c;g_result_iters=0;return;}
    g_result_row=pool[bci].mr;
    g_result_col=pool[bci].mc;
    g_result_iters=max_iters;
}

// ── 匯出給 JS 的 C 介面 ───────────────────────────────────────────────
extern "C" {

__attribute__((export_name("init")))
void init(){
    build_lines();
    rng_seed(12345678u);
    memset(g_board,0,sizeof(g_board));
}

__attribute__((export_name("clear_board")))
void clear_board(){
    memset(g_board,0,sizeof(g_board));
}

__attribute__((export_name("set_cell")))
void set_cell(int r,int c,int v){
    if(r>=0&&r<N&&c>=0&&c<N) g_board[r][c]=(int8_t)v;
}

__attribute__((export_name("get_cell")))
int get_cell(int r,int c){
    if(r>=0&&r<N&&c>=0&&c<N) return g_board[r][c];
    return 0;
}

__attribute__((export_name("best_move")))
void best_move(int player, int time_ms){
    uct_move(player, time_ms);
}

__attribute__((export_name("result_row")))
int result_row(){ return g_result_row; }

__attribute__((export_name("result_col")))
int result_col(){ return g_result_col; }

__attribute__((export_name("result_iters")))
int result_iters(){ return g_result_iters; }

__attribute__((export_name("seed_rng")))
void seed_rng(int s){ rng_seed((uint32_t)s); }

} // extern "C"
