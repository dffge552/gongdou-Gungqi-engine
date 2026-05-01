/*
 * gongqi_server.cpp  ─  宮棋 V1 MCTS 本地 HTTP Server
 *
 * 編譯（MinGW g++）：
 *   g++ -O2 -std=c++17 -o gongqi_server.exe gongqi_server.cpp -lws2_32
 *
 * 執行：
 *   gongqi_server.exe          (預設 port 8765)
 *   gongqi_server.exe 9000     (自訂 port)
 *
 * HTML 透過 fetch 呼叫：
 *   POST http://localhost:8765/move
 *   Body (JSON):
 *     {
 *       "board": [[0,0,...], ...],   // 10x10，0=空 1=黑 -1=白
 *       "player": 1,                 // 1=黑 -1=白
 *       "time_limit": 3.0            // 思考秒數
 *     }
 *   Response (JSON):
 *     { "row": 3, "col": 5, "iters": 14000 }
 */

#ifndef _WIN32
#error "此檔案僅供 Windows (MinGW) 使用，Linux 請改用 POSIX socket。"
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bits/stdc++.h>
#pragma comment(lib,"ws2_32.lib")
using namespace std;

// ════════════════════════════════════════════════
//  宮棋核心邏輯
// ════════════════════════════════════════════════

static const int N     = 10;
static const int EMPTY =  0;
static const int BLACK =  1;
static const int WHITE = -1;

using Board = array<array<int,N>,N>;
using Pos   = pair<int,int>;
using Line  = vector<Pos>;

static vector<Line> ALL_LINES;
static vector<int>  CELL_LINES[N][N];

void build_lines(){
    auto add=[&](Line line){
        if((int)line.size()<3) return;
        int idx=(int)ALL_LINES.size();
        ALL_LINES.push_back(line);
        for(auto [r,c]:line) CELL_LINES[r][c].push_back(idx);
    };
    for(int r=0;r<N;r++){Line l;for(int c=0;c<N;c++)l.push_back({r,c});add(l);}
    for(int c=0;c<N;c++){Line l;for(int r=0;r<N;r++)l.push_back({r,c});add(l);}
    for(int s=-(N-1);s<N;s++){
        Line l;for(int i=0;i<N;i++){int r=s+i,c=i;if(r>=0&&r<N)l.push_back({r,c});}add(l);}
    for(int s=0;s<2*N-1;s++){
        Line l;for(int i=0;i<N;i++){int r=s-i,c=i;if(r>=0&&r<N)l.push_back({r,c});}add(l);}
}

struct Group{ int start,len; vector<Pos> pos; };

vector<Group> find_groups(const Line& line,const Board& board,int player){
    vector<pair<int,Pos>> stones;
    for(int i=0;i<(int)line.size();i++)
        if(board[line[i].first][line[i].second]==player)
            stones.push_back({i,line[i]});
    if(stones.empty()) return {};
    vector<Group> groups; int s=0;
    for(int k=1;k<=(int)stones.size();k++){
        if(k==(int)stones.size()||stones[k].first!=stones[k-1].first+1){
            int len=k-s;
            if(len>=2){
                Group g; g.start=stones[s].first; g.len=len;
                for(int j=s;j<k;j++) g.pos.push_back(stones[j].second);
                groups.push_back(g);
            }
            s=k;
        }
    }
    return groups;
}

vector<Pos> find_parallel(const Line&,const vector<Pos>& positions,
                           const Board& board,int opponent,int count,bool both=false){
    if((int)positions.size()<2) return {};
    int dr=positions[1].first-positions[0].first;
    int dc=positions[1].second-positions[0].second;
    vector<pair<int,int>> perp;
    if(dr==0) perp={{-1,0},{1,0}};
    else if(dc==0) perp={{0,-1},{0,1}};
    else perp={{-dr,dc},{dr,-dc}};
    vector<Pos> removed;
    for(auto [pdr,pdc]:perp){
        vector<Pos> cands; bool valid=true;
        for(auto [r,c]:positions){
            int nr=r+pdr,nc=c+pdc;
            if(nr<0||nr>=N||nc<0||nc>=N||board[nr][nc]!=opponent){valid=false;break;}
            cands.push_back({nr,nc});
        }
        if(valid&&(int)cands.size()==count){
            for(auto p:cands) removed.push_back(p);
            if(!both) break;
        }
    }
    return removed;
}

bool is_protected(const Board& board,Pos pos){
    int player=board[pos.first][pos.second];
    if(player==EMPTY) return false;
    for(int li:CELL_LINES[pos.first][pos.second])
        for(auto& g:find_groups(ALL_LINES[li],board,player))
            if(g.len>=3&&find(g.pos.begin(),g.pos.end(),pos)!=g.pos.end())
                return true;
    return false;
}

pair<int,int> calculate_captures(Board board){
    int bc=0,wc=0;
    for(int pr:{6,5,4,3}){
        vector<pair<int,vector<Pos>>> actions;
        for(auto& line:ALL_LINES){
            for(int pl:{BLACK,WHITE}){
                for(auto& g:find_groups(line,board,pl)){
                    if(g.len!=pr) continue;
                    int opp=-pl; vector<Pos> rm;
                    if(pr==3){
                        if(g.start>0){Pos lp=line[g.start-1];if(board[lp.first][lp.second]==opp)rm.push_back(lp);}
                        int ri=g.start+g.len;if(ri<(int)line.size()){Pos rp=line[ri];if(board[rp.first][rp.second]==opp)rm.push_back(rp);}
                        if(!rm.empty()) rm={rm[0]};
                    } else if(pr==4){
                        if(g.start>0){Pos lp=line[g.start-1];if(board[lp.first][lp.second]==opp)rm.push_back(lp);}
                        int ri=g.start+g.len;if(ri<(int)line.size()){Pos rp=line[ri];if(board[rp.first][rp.second]==opp)rm.push_back(rp);}
                    } else if(pr==5){
                        for(auto p:find_parallel(line,g.pos,board,opp,5)) rm.push_back(p);
                    } else {
                        for(auto p:find_parallel(line,g.pos,board,opp,6,true)) rm.push_back(p);
                    }
                    if(!rm.empty()){
                        sort(rm.begin(),rm.end());
                        rm.erase(unique(rm.begin(),rm.end()),rm.end());
                        actions.push_back({pl,rm});
                    }
                }
            }
        }
        for(auto& [pl,positions]:actions)
            for(auto pos:positions){
                if(board[pos.first][pos.second]!=-pl) continue;
                if(is_protected(board,pos)) continue;
                board[pos.first][pos.second]=EMPTY;
                if(pl==BLACK) bc++; else wc++;
            }
    }
    return {bc,wc};
}

vector<Pos> get_empty(const Board& b){
    vector<Pos> v;
    for(int r=0;r<N;r++) for(int c=0;c<N;c++) if(b[r][c]==EMPTY) v.push_back({r,c});
    return v;
}
pair<int,int> get_remaining(const Board& b){
    int bp=0,wp=0;
    for(int r=0;r<N;r++) for(int c=0;c<N;c++){if(b[r][c]==BLACK)bp++;if(b[r][c]==WHITE)wp++;}
    return {50-bp,50-wp};
}

static mt19937 rng(chrono::steady_clock::now().time_since_epoch().count());

float rollout_v1(Board board,int player){
    auto empty=get_empty(board);
    shuffle(empty.begin(),empty.end(),rng);
    auto [br,wr]=get_remaining(board);
    int cur=player;
    for(auto [r,c]:empty){
        if(cur==BLACK&&br>0){board[r][c]=BLACK;br--;}
        else if(cur==WHITE&&wr>0){board[r][c]=WHITE;wr--;}
        else if(br>0){board[r][c]=BLACK;br--;}
        else{board[r][c]=WHITE;wr--;}
        cur=-cur;
    }
    auto [bc,wc]=calculate_captures(board);
    return ((player==BLACK)?(float)(bc-wc):(float)(wc-bc))/50.f;
}

struct Node{
    Pos move; int player,parent;
    vector<int> children;
    vector<Pos> untried;
    int visits=0; float value=0.f,rval=0.f; int rv=0;
    float score(float c,int pv,int k=300)const{
        if(visits==0) return 1e9f;
        float ex=value/visits;
        if(rv>0){float rave=rval/rv,beta=sqrtf((float)k/(3.f*visits+k));ex=(1-beta)*ex+beta*rave;}
        return ex+c*sqrtf(logf((float)max(pv,1))/(float)visits);
    }
};

// 回傳 {row, col, iters}
tuple<int,int,int> uct_move_v1(const Board& board,int player,double time_limit,float c=1.f){
    auto empty=get_empty(board);
    if(empty.empty()) return {-1,-1,0};
    if((int)empty.size()==1) return {empty[0].first,empty[0].second,1};

    vector<Node> pool; pool.reserve(1<<20);
    auto new_node=[&](Pos mv,int pl,int par,vector<Pos> un)->int{
        int idx=(int)pool.size(); pool.push_back({});
        auto& nd=pool.back();
        nd.move=mv;nd.player=pl;nd.parent=par;nd.untried=std::move(un);
        return idx;
    };
    shuffle(empty.begin(),empty.end(),rng);
    int root=new_node({-1,-1},-player,-1,empty);
    auto deadline=chrono::steady_clock::now()+chrono::duration<double>(time_limit);
    int iters=0;

    while(chrono::steady_clock::now()<deadline){
        int ni=root; Board sim=board; int cur=player;
        while(pool[ni].untried.empty()&&!pool[ni].children.empty()){
            int bc2=-1; float bs=-1e9f; int pv=max(pool[ni].visits,1);
            for(int ci:pool[ni].children){float s=pool[ci].score(c,pv);if(s>bs){bs=s;bc2=ci;}}
            ni=bc2;
            auto [r2,c2]=pool[ni].move; if(r2>=0) sim[r2][c2]=cur; cur=-cur;
        }
        if(!pool[ni].untried.empty()){
            int pick=rng()%pool[ni].untried.size();
            Pos mv=pool[ni].untried[pick];
            pool[ni].untried.erase(pool[ni].untried.begin()+pick);
            sim[mv.first][mv.second]=cur;
            auto un=get_empty(sim); shuffle(un.begin(),un.end(),rng);
            int ci=new_node(mv,-cur,ni,std::move(un));
            pool[ni].children.push_back(ci); ni=ci; cur=-cur;
        }
        float reward=rollout_v1(sim,cur); if(cur!=player) reward=-reward;
        for(int n=ni;n!=-1;n=pool[n].parent){
            pool[n].visits++;
            if(pool[n].parent!=-1){
                int mover=-pool[n].player;
                float r2=(mover==player)?reward:-reward;
                pool[n].value+=r2; pool[n].rv++; pool[n].rval+=r2;
            }
        }
        iters++;
    }
    if(pool[root].children.empty()){auto p=empty[rng()%empty.size()];return{p.first,p.second,iters};}
    int bci=-1,bv=-1;
    for(int ci:pool[root].children) if(pool[ci].visits>bv){bv=pool[ci].visits;bci=ci;}
    auto [br2,bc2]=pool[bci].move;
    return {br2,bc2,iters};
}

// ════════════════════════════════════════════════
//  簡易 JSON 解析（不依賴外部庫）
// ════════════════════════════════════════════════

// 從 JSON body 解析 board / player / time_limit
// 格式嚴格遵循上方文件，不做容錯
bool parse_request(const string& body, Board& board, int& player, double& time_limit){
    try{
        // player
        auto ppos=body.find("\"player\"");
        if(ppos==string::npos) return false;
        ppos=body.find(':',ppos)+1;
        while(ppos<body.size()&&isspace(body[ppos])) ppos++;
        player=stoi(body.substr(ppos));

        // time_limit
        auto tpos=body.find("\"time_limit\"");
        if(tpos==string::npos) return false;
        tpos=body.find(':',tpos)+1;
        while(tpos<body.size()&&isspace(body[tpos])) tpos++;
        time_limit=stod(body.substr(tpos));

        // board: 找到第一個 '[' 之後的 10x10 整數
        auto bpos=body.find("\"board\"");
        if(bpos==string::npos) return false;
        bpos=body.find('[',bpos)+1; // 外層 [
        for(int r=0;r<N;r++){
            bpos=body.find('[',bpos)+1; // 行 [
            for(int c=0;c<N;c++){
                while(bpos<body.size()&&(isspace(body[bpos])||body[bpos]==',')) bpos++;
                int sign=1;
                if(body[bpos]=='-'){sign=-1;bpos++;}
                int v=0;
                while(bpos<body.size()&&isdigit(body[bpos])){v=v*10+(body[bpos]-'0');bpos++;}
                board[r][c]=sign*v;
            }
            bpos=body.find(']',bpos)+1; // 行 ]
        }
        return true;
    } catch(...){ return false; }
}

// ════════════════════════════════════════════════
//  HTTP Server
// ════════════════════════════════════════════════

void send_response(SOCKET sock, int status, const string& content_type, const string& body){
    string status_text = (status==200)?"OK":"Bad Request";
    string resp =
        "HTTP/1.1 " + to_string(status) + " " + status_text + "\r\n"
        "Content-Type: " + content_type + "\r\n"
        "Content-Length: " + to_string(body.size()) + "\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: POST, OPTIONS\r\n"
        "Access-Control-Allow-Headers: Content-Type\r\n"
        "Connection: close\r\n"
        "\r\n" + body;
    send(sock, resp.c_str(), (int)resp.size(), 0);
}

void handle_client(SOCKET sock){
    // 讀取請求
    string req;
    char buf[4096];
    int n;
    while((n=recv(sock,buf,sizeof(buf)-1,0))>0){
        buf[n]=0; req+=buf;
        if(req.find("\r\n\r\n")!=string::npos) break;
    }
    // 繼續讀 body（根據 Content-Length）
    auto cl_pos=req.find("Content-Length:");
    if(cl_pos!=string::npos){
        int cl=stoi(req.substr(cl_pos+15));
        auto hend=req.find("\r\n\r\n");
        int body_have=(int)req.size()-(int)(hend+4);
        while(body_have<cl&&(n=recv(sock,buf,sizeof(buf)-1,0))>0){
            buf[n]=0; req+=buf; body_have+=n;
        }
    }

    // OPTIONS preflight
    if(req.substr(0,7)=="OPTIONS"){
        send_response(sock,200,"text/plain","");
        closesocket(sock); return;
    }

    // 只處理 POST /move
    if(req.substr(0,4)!="POST"){
        send_response(sock,400,"text/plain","Only POST /move is supported.");
        closesocket(sock); return;
    }

    auto hend=req.find("\r\n\r\n");
    string body = (hend!=string::npos) ? req.substr(hend+4) : "";

    Board board{}; int player=1; double time_limit=3.0;
    if(!parse_request(body,board,player,time_limit)){
        send_response(sock,400,"application/json","{\"error\":\"invalid request\"}");
        closesocket(sock); return;
    }

    auto [row,col,iters]=uct_move_v1(board,player,time_limit);
    fprintf(stderr,"[V1] iters=%d best=(%d,%d)\n",iters,row,col);

    string resp_body =
        "{\"row\":" + to_string(row) +
        ",\"col\":" + to_string(col) +
        ",\"iters\":" + to_string(iters) + "}";
    send_response(sock,200,"application/json",resp_body);
    closesocket(sock);
}

int main(int argc,char** argv){
    build_lines();

    int port = (argc>=2) ? atoi(argv[1]) : 8765;

    WSADATA wsa;
    WSAStartup(MAKEWORD(2,2),&wsa);

    SOCKET srv=socket(AF_INET,SOCK_STREAM,0);
    int opt=1; setsockopt(srv,SOL_SOCKET,SO_REUSEADDR,(char*)&opt,sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family=AF_INET;
    addr.sin_addr.s_addr=INADDR_ANY;
    addr.sin_port=htons((u_short)port);
    if(bind(srv,(sockaddr*)&addr,sizeof(addr))==SOCKET_ERROR){
        fprintf(stderr,"bind failed: %d\n",WSAGetLastError());
        return 1;
    }
    listen(srv,10);
    fprintf(stderr,"宮棋 AI server 啟動，port %d\n",port);
    fprintf(stderr,"在瀏覽器打開 gongqi.html 即可開始遊戲\n");

    while(true){
        SOCKET client=accept(srv,nullptr,nullptr);
        if(client==INVALID_SOCKET) continue;
        // 單執行緒：逐一處理（夠用，AI思考期間不需要並發）
        handle_client(client);
    }
    WSACleanup();
    return 0;
}
