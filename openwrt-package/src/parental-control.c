#define _GNU_SOURCE
#include <json-c/json.h>
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <poll.h>

#define DEVICES "/etc/parental-control/devices.json"
#define SOCK "/var/run/parental-control.sock"
#define STATE "/var/lib/parental-control/state.json"
#define HISTORY "/var/lib/parental-control/history.json"
#define NFT_RULES "/var/run/parental-control.nft"
static json_object *arr(void); static json_object *okmsg(const char*); static int sync_dhcp_macs(void); static const char *sval(json_object*,const char*,const char*); static int ival(json_object*,const char*,int); static int bval(json_object*,const char*,int); static int findid(const char*); static void dhcp(json_object*,char*,size_t); static int night(json_object*); static int nft_apply(void); static int nft_commit(void); static void state_device(json_object*,json_object*,int); static json_object *state_snapshot(void);
static json_object *root; static int manual[128],break_active[128],temporary_unblock[128]; static time_t brk_until[128];
static long long used_seconds[128], session_seconds[128], bonus_minutes[128];
static unsigned long long previous_inbound[128], previous_outbound[128];
static int counters_seen[128], blocked_cache[128], nft_dirty=1; static time_t last_active[128];
static char reset_date[16]="";
static void ensure_varlib(void){mkdir("/var/lib",0755);mkdir("/var/lib/parental-control",0755);}
static void date_now(char out[16]){time_t t=time(NULL);struct tm z;localtime_r(&t,&z);strftime(out,16,"%F",&z);}
static ssize_t read_http_request(int fd,char*b,size_t cap){size_t used=0,need=0;while(used+1<cap){ssize_t r=read(fd,b+used,cap-used-1);if(r<0){if(errno==EINTR)continue;return-1;}if(!r)break;used+=(size_t)r;b[used]=0;char*sep=strstr(b,"\r\n\r\n");if(!sep)continue;size_t header_len=(size_t)(sep+4-b);if(!need){size_t body_len=0;char*cl=strcasestr(b,"Content-Length:");if(cl&&cl<sep){cl+=15;while(*cl==' '||*cl=='\t')cl++;char*end=NULL;unsigned long long v=strtoull(cl,&end,10);if(end==cl||v>cap-1-header_len)return-2;body_len=(size_t)v;}need=header_len+body_len;}if(used>=need)return(ssize_t)used;}return used?(ssize_t)used:-1;}
static void save_state(void){
    ensure_varlib();
    json_object *j=state_snapshot();
    if(!j)return;
    char tmp[256];snprintf(tmp,sizeof tmp,"%s.tmp",STATE);
    if(json_object_to_file_ext(tmp,j,JSON_C_TO_STRING_PRETTY)==0)rename(tmp,STATE);
    else unlink(tmp);
    json_object_put(j);
}
static void load_state(void){
    char today[16];date_now(today);
    json_object*j=json_object_from_file(STATE);
    if(!j){snprintf(reset_date,sizeof reset_date,"%s",today);return;}

    /* Compatibility with the older internal state format. */
    json_object*v=NULL,*a=NULL;
    if(json_object_object_get_ex(j,"states",&a)&&json_object_is_type(a,json_type_array)){
        if(json_object_object_get_ex(j,"reset_date",&v))snprintf(reset_date,sizeof reset_date,"%s",json_object_get_string(v));
        if(strcmp(reset_date,today)){snprintf(reset_date,sizeof reset_date,"%s",today);json_object_put(j);return;}
        for(int x=0;x<(int)json_object_array_length(a);x++){
            json_object*o=json_object_array_get_idx(a,x);int i=findid(sval(o,"id",""));if(i<0)continue;
            used_seconds[i]=ival(o,"used_seconds",0);
            session_seconds[i]=ival(o,"session_seconds",0);
            brk_until[i]=(time_t)json_object_get_int64(json_object_object_get(o,"break_until"));
            manual[i]=bval(o,"manual_block",0);
            temporary_unblock[i]=bval(o,"temporary_unblock",0);
            bonus_minutes[i]=ival(o,"bonus_minutes",0);
            last_active[i]=(time_t)json_object_get_int64(json_object_object_get(o,"last_active"));
            previous_inbound[i]=(unsigned long long)json_object_get_int64(json_object_object_get(o,"previous_inbound"));
            previous_outbound[i]=(unsigned long long)json_object_get_int64(json_object_object_get(o,"previous_outbound"));
            counters_seen[i]=bval(o,"counters_seen",0);
            break_active[i]=brk_until[i]>time(NULL);
        }
        json_object_put(j);return;
    }

    /* Current format: the same JSON object that /api/state returns. */
    time_t saved_at=time(NULL);
    if(json_object_object_get_ex(j,"state_timestamp",&v))saved_at=(time_t)json_object_get_int64(v);
    struct tm z;localtime_r(&saved_at,&z);char saved_date[16];strftime(saved_date,sizeof saved_date,"%F",&z);
    snprintf(reset_date,sizeof reset_date,"%s",today);
    if(strcmp(saved_date,today)){json_object_put(j);return;}

    if(json_object_object_get_ex(j,"devices",&a)&&json_object_is_type(a,json_type_array)){
        for(int x=0;x<(int)json_object_array_length(a);x++){
            json_object*o=json_object_array_get_idx(a,x);int i=findid(sval(o,"id",""));if(i<0)continue;
            used_seconds[i]=ival(o,"used_seconds",0);
            session_seconds[i]=ival(o,"session_used_seconds",0);
            manual[i]=bval(o,"manual_blocked",0);
            temporary_unblock[i]=bval(o,"temporary_unblock",0);
            bonus_minutes[i]=ival(o,"bonus_minutes",0);
            int ba=bval(o,"break_active",0),left=ival(o,"break_remaining_seconds",0);
            time_t elapsed=time(NULL)>saved_at?time(NULL)-saved_at:0;if(elapsed>0&&left>0)left=elapsed>=left?0:left-(int)elapsed;
            break_active[i]=ba&&left>0;
            brk_until[i]=break_active[i]?time(NULL)+left:0;
            counters_seen[i]=0;
            previous_inbound[i]=previous_outbound[i]=0;
            last_active[i]=0;
        }
    }
    json_object_put(j);
}
static void reset_today_all(void){char today[16];date_now(today);snprintf(reset_date,sizeof reset_date,"%s",today);memset(used_seconds,0,sizeof used_seconds);memset(session_seconds,0,sizeof session_seconds);memset(bonus_minutes,0,sizeof bonus_minutes);memset(break_active,0,sizeof break_active);memset(temporary_unblock,0,sizeof temporary_unblock);memset(brk_until,0,sizeof brk_until);save_state();}
static int read_nft_counters(unsigned long long *inb,unsigned long long *outb){
    memset(inb,0,sizeof(unsigned long long)*128); memset(outb,0,sizeof(unsigned long long)*128);
    FILE *f=popen("(/usr/sbin/nft list chain inet parental_control pc_forward 2>/dev/null || /usr/bin/nft list chain inet parental_control pc_forward 2>/dev/null)","r");
    if (!f) return 0;
    char line[2048];
    int found=0;
    while(fgets(line,sizeof line,f)){
        char *c=strstr(line,"comment \"pc:"); if(!c) continue;
        char *bp=strstr(line," bytes "); if(!bp) continue; unsigned long long bytes=strtoull(bp+7,NULL,10);
        c+=12; char *e=strchr(c,'\"'); if(!e)continue; *e=0; char *dir=strrchr(c,':'); if(!dir)continue; *dir++=0;
        int i=findid(c); if(i<0)continue; if(!strcmp(dir,"in"))inb[i]=bytes; else if(!strcmp(dir,"out"))outb[i]=bytes; else continue; found=1;
    } pclose(f); return found;
}
static int is_blocked(json_object*d,int i){if(!bval(d,"enabled",1))return 0;long long lim=(long long)ival(d,"daily_limit_minutes",0)*60+bonus_minutes[i]*60;return manual[i]||(!temporary_unblock[i]&&(night(d)||(bval(d,"break_enabled",0)&&break_active[i]&&time(NULL)<brk_until[i])||(bval(d,"daily_limit_enabled",0)&&lim>0&&used_seconds[i]>=lim)));}
static void tick(void){static time_t last_dhcp_sync=0;time_t now_sync=time(NULL);if(now_sync-last_dhcp_sync>=2){sync_dhcp_macs();last_dhcp_sync=now_sync;}
    static time_t last=0; time_t now=time(NULL); if(!last){last=now;return;} int dt=(int)(now-last); if(dt<1)return; if(dt>60)dt=1; last=now;
    char today[16]; date_now(today); if(strcmp(today,reset_date))reset_today_all();
    unsigned long long inb[128],outb[128]; int have=read_nft_counters(inb,outb); json_object*a=arr(); int changed=0, rules_changed=0;
    for(int i=0;i<(int)json_object_array_length(a)&&i<128;i++){
        json_object*d=json_object_array_get_idx(a,i); if(break_active[i]&&now>=brk_until[i]){break_active[i]=0;brk_until[i]=0;session_seconds[i]=0;changed=rules_changed=1;}
        int before=is_blocked(d,i),break_enabled=bval(d,"break_enabled",0),cycle=bval(d,"session_limit_enabled",0)&&break_enabled,active=0;
        if(!cycle&&session_seconds[i]){session_seconds[i]=0;changed=1;}
        if(!break_enabled&&(break_active[i]||brk_until[i])){break_active[i]=0;brk_until[i]=0;changed=1;if(before)rules_changed=1;}
        if(have){
            if(counters_seen[i]){unsigned long long di=inb[i]>=previous_inbound[i]?inb[i]-previous_inbound[i]:inb[i]; unsigned long long do_=outb[i]>=previous_outbound[i]?outb[i]-previous_outbound[i]:outb[i]; unsigned long long threshold=(unsigned long long)ival(d,"activity_threshold_bytes",4096); if(!before&&di+do_>=threshold){active=1;last_active[i]=now;}}
            else {counters_seen[i]=1;}
            previous_inbound[i]=inb[i]; previous_outbound[i]=outb[i]; changed=1;
        }
        int idle=ival(d,"idle_timeout_seconds",300); if(!before&&!active&&last_active[i]&&idle>0&&now-last_active[i]<=idle)active=1;
        if(!bval(d,"enabled",1))active=0;
        if(active&&!before){used_seconds[i]+=dt;changed=1;if(cycle){session_seconds[i]+=dt;int lim=ival(d,"session_limit_minutes",0)*60;if(lim>0&&session_seconds[i]>=lim){break_active[i]=1;brk_until[i]=now+ival(d,"break_minutes",0)*60;session_seconds[i]=0;changed=rules_changed=1;}}}
        int after=is_blocked(d,i); if(after!=before||after!=blocked_cache[i])rules_changed=1;
    }
    if(changed)save_state();
    if(rules_changed)nft_dirty=1;
    if(nft_dirty)nft_commit();
}

static void save(void){char tmp[256];snprintf(tmp,sizeof tmp,"%s.tmp",DEVICES);if(json_object_to_file_ext(tmp,root,JSON_C_TO_STRING_PRETTY)==0)rename(tmp,DEVICES);else unlink(tmp);}
static void runtime_remove(int i){if(i<0||i>=128)return;for(int k=i;k<127;k++){manual[k]=manual[k+1];break_active[k]=break_active[k+1];temporary_unblock[k]=temporary_unblock[k+1];brk_until[k]=brk_until[k+1];used_seconds[k]=used_seconds[k+1];session_seconds[k]=session_seconds[k+1];bonus_minutes[k]=bonus_minutes[k+1];previous_inbound[k]=previous_inbound[k+1];previous_outbound[k]=previous_outbound[k+1];counters_seen[k]=counters_seen[k+1];blocked_cache[k]=blocked_cache[k+1];last_active[k]=last_active[k+1];}manual[127]=break_active[127]=temporary_unblock[127]=counters_seen[127]=blocked_cache[127]=0;brk_until[127]=last_active[127]=0;used_seconds[127]=session_seconds[127]=bonus_minutes[127]=0;previous_inbound[127]=previous_outbound[127]=0;}
static void make_device_id(const char*mac,char out[64]){char*p=out;memcpy(p,"device-",7);p+=7;for(const unsigned char*s=(const unsigned char*)mac;*s&&p<out+63;s++)if(isxdigit(*s))*p++=(char)tolower(*s);*p=0;}
static json_object *arr(void){json_object *a=NULL;if(!root||!json_object_object_get_ex(root,"devices",&a)){root=json_object_new_object();a=json_object_new_array();json_object_object_add(root,"devices",a);}return a;}
static const char *sval(json_object *o,const char*k,const char*d){json_object*v;return json_object_object_get_ex(o,k,&v)?json_object_get_string(v):d;}
static int ival(json_object *o,const char*k,int d){json_object*v;return json_object_object_get_ex(o,k,&v)?json_object_get_int(v):d;}
static int bval(json_object *o,const char*k,int d){json_object*v;return json_object_object_get_ex(o,k,&v)?json_object_get_boolean(v):d;}
static int findid(const char *id){json_object*a=arr();for(int i=0;i<(int)json_object_array_length(a);i++)if(!strcmp(sval(json_object_array_get_idx(a,i),"id",""),id))return i;return -1;}
static void dhcp(json_object*d,char *ip,size_t ni){ip[0]=0;FILE*f=fopen("/tmp/dhcp.leases","r");if(!f)return;char line[512],mac[32],dip[64],host[128];const char *want=sval(d,"mac","");while(fgets(line,sizeof line,f)){long x;if(sscanf(line,"%ld %31s %63s %127s",&x,mac,dip,host)>=3&&!strcasecmp(mac,want)){snprintf(ip,ni,"%s",dip);break;}}fclose(f);}
static int time_valid(const char*s){if(!s||strlen(s)!=5||s[2]!=':'||!isdigit((unsigned char)s[0])||!isdigit((unsigned char)s[1])||!isdigit((unsigned char)s[3])||!isdigit((unsigned char)s[4]))return 0;int h=(s[0]-'0')*10+(s[1]-'0'),m=(s[3]-'0')*10+(s[4]-'0');return h>=0&&h<24&&m>=0&&m<60;}
static int night(json_object*d){if(!bval(d,"night_enabled",0))return 0;const char*start=sval(d,"night_start","22:00"),*end=sval(d,"night_end","08:00");if(!time_valid(start)||!time_valid(end))return 0;int sh,sm,eh,em;sscanf(start,"%d:%d",&sh,&sm);sscanf(end,"%d:%d",&eh,&em);time_t t=time(NULL);struct tm z;localtime_r(&t,&z);int n=z.tm_hour*60+z.tm_min,a=sh*60+sm,b=eh*60+em;return a<=b?(n>=a&&n<b):(n>=a||n<b);}
static void state_device(json_object*out,json_object*d,int i){
    json_object_object_add(out,"id",json_object_new_string(sval(d,"id","")));
    json_object_object_add(out,"name",json_object_new_string(sval(d,"name","")));

    json_object *h=NULL;
    const char *hostname=sval(d,"hostname","");
    if(json_object_object_get_ex(d,"hostnames",&h)&&json_object_is_type(h,json_type_array)&&json_object_array_length(h)>0)
        hostname=json_object_get_string(json_object_array_get_idx(h,0));
    json_object_object_add(out,"hostname",json_object_new_string(hostname));
    if(h)json_object_object_add(out,"hostnames",json_object_get(h));
    else {json_object*hs=json_object_new_array();if(*hostname)json_object_array_add(hs,json_object_new_string(hostname));json_object_object_add(out,"hostnames",hs);}

    json_object_object_add(out,"mac",json_object_new_string(sval(d,"mac","")));
    char ip[64];dhcp(d,ip,sizeof ip);if(*ip)json_object_object_add(out,"ip",json_object_new_string(ip));
    json_object_object_add(out,"enabled",json_object_new_boolean(bval(d,"enabled",1)));

    const char *keys[]={"daily_limit_minutes","session_limit_minutes","break_minutes","daily_limit_enabled","session_limit_enabled","break_enabled","night_enabled","night_start","night_end"};
    for(size_t k=0;k<sizeof(keys)/sizeof(keys[0]);k++){json_object*v;if(json_object_object_get_ex(d,keys[k],&v))json_object_object_add(out,keys[k],json_object_get(v));}

    int n=night(d),ba=bval(d,"break_enabled",0)&&break_active[i]&&time(NULL)<brk_until[i];
    int daily_limit=ival(d,"daily_limit_minutes",0)*60+(int)bonus_minutes[i]*60;
    int dl=bval(d,"daily_limit_enabled",0)&&daily_limit>0&&used_seconds[i]>=daily_limit;
    int enabled=bval(d,"enabled",1),blocked=is_blocked(d,i);
    long long session_limit=(long long)ival(d,"session_limit_minutes",60)*60;
    long long session_remaining=session_limit-session_seconds[i];if(session_remaining<0)session_remaining=0;
    long long daily_remaining=(long long)daily_limit-used_seconds[i];if(daily_remaining<0)daily_remaining=0;

    json_object_object_add(out,"used_seconds",json_object_new_int64(used_seconds[i]));
    json_object_object_add(out,"daily_limit_seconds",json_object_new_int(daily_limit));
    json_object_object_add(out,"daily_remaining_seconds",json_object_new_int64(daily_remaining));
    json_object_object_add(out,"session_used_seconds",json_object_new_int64(session_seconds[i]));
    json_object_object_add(out,"session_limit_seconds",json_object_new_int64(session_limit));
    json_object_object_add(out,"session_remaining_seconds",json_object_new_int64(session_remaining));
    long long break_remaining=ba?(long long)(brk_until[i]-time(NULL)):0;if(break_remaining<0)break_remaining=0;
    json_object_object_add(out,"break_active",json_object_new_boolean(ba));
    json_object_object_add(out,"break_remaining_seconds",json_object_new_int64(break_remaining));
    json_object_object_add(out,"bonus_minutes",json_object_new_int64(bonus_minutes[i]));
    json_object_object_add(out,"temporary_unblock",json_object_new_boolean(temporary_unblock[i]));
    json_object_object_add(out,"manual_blocked",json_object_new_boolean(manual[i]));
    json_object_object_add(out,"blocked",json_object_new_boolean(blocked));
    json_object_object_add(out,"access_allowed",json_object_new_boolean(!blocked));

    json_object_object_add(out,"block_reason",json_object_new_string(blocked?(manual[i]?"manual":n&&!temporary_unblock[i]?"night":ba&&!temporary_unblock[i]?"break":"daily_limit"):""));
    json_object *rs=json_object_new_array();
    if(enabled&&manual[i])json_object_array_add(rs,json_object_new_string("manual"));
    if(enabled&&n&&!temporary_unblock[i])json_object_array_add(rs,json_object_new_string("night"));
    if(enabled&&ba&&!temporary_unblock[i])json_object_array_add(rs,json_object_new_string("break"));
    if(enabled&&dl&&!temporary_unblock[i])json_object_array_add(rs,json_object_new_string("daily_limit"));
    json_object_object_add(out,"block_reasons",rs);
}
static json_object *state_snapshot(void){
    json_object*j=okmsg("ok"),*ds=json_object_new_array();
    json_object*a=arr();
    for(int i=0;i<(int)json_object_array_length(a);i++){
        json_object*o=json_object_new_object();
        state_device(o,json_object_array_get_idx(a,i),i);
        json_object_array_add(ds,o);
    }
    json_object_object_add(j,"devices",ds);
    json_object_object_add(j,"state_timestamp",json_object_new_int64(time(NULL)));
    return j;
}
static int nft_apply(void){FILE*f=fopen(NFT_RULES,"w");if(!f)return -1;fprintf(f,"destroy table inet parental_control\nadd table inet parental_control\nadd chain inet parental_control pc_forward { type filter hook forward priority -5; policy accept; }\n");json_object*a=arr();for(int i=0;i<(int)json_object_array_length(a)&&i<128;i++){json_object*d=json_object_array_get_idx(a,i);int block=is_blocked(d,i);const char*m=sval(d,"mac","");const char*id=sval(d,"id","");fprintf(f,"add rule inet parental_control pc_forward ether saddr %s counter comment \"pc:%s:out\"\n",m,id);fprintf(f,"add rule inet parental_control pc_forward ether daddr %s counter comment \"pc:%s:in\"\n",m,id);if(block){fprintf(f,"add rule inet parental_control pc_forward ether saddr %s drop comment \"pc:%s:forward-src\"\n",m,id);fprintf(f,"add rule inet parental_control pc_forward ether daddr %s drop comment \"pc:%s:forward-dst\"\n",m,id);}}fclose(f);int rc=system("/usr/sbin/nft -f " NFT_RULES " >/dev/null 2>&1 || /usr/bin/nft -f " NFT_RULES " >/dev/null 2>&1");return rc;}
static int nft_commit(void){int rc=nft_apply();if(rc==0){nft_dirty=0;json_object*a=arr();for(int i=0;i<(int)json_object_array_length(a)&&i<128;i++)blocked_cache[i]=is_blocked(json_object_array_get_idx(a,i),i);}else{nft_dirty=1;fprintf(stderr,"parental-control: nftables apply failed; will retry\n");}return rc;}

static int mac_valid(const char *m){unsigned x[6];char tail;return m&&sscanf(m,"%2x:%2x:%2x:%2x:%2x:%2x%c",&x[0],&x[1],&x[2],&x[3],&x[4],&x[5],&tail)==6;}
static int id_valid(const char*id){if(!id||!*id||strlen(id)>63)return 0;for(const unsigned char*p=(const unsigned char*)id;*p;p++)if(!(isalnum(*p)||*p=='-'||*p=='_'||*p=='.'))return 0;return 1;}
static int hostname_valid(const char *h){if(!h||!*h||strlen(h)>127)return 0;for(const unsigned char*p=(const unsigned char*)h;*p;p++)if(!(isalnum(*p)||*p=='-'||*p=='_'||*p=='.'))return 0;return 1;}
static int known_mac(const char *mac){json_object*a=arr();for(int i=0;i<(int)json_object_array_length(a);i++)if(!strcasecmp(sval(json_object_array_get_idx(a,i),"mac",""),mac))return 1;return 0;}
static int known_hostname(const char *host){json_object*a=arr();for(int i=0;i<(int)json_object_array_length(a);i++){json_object*d=json_object_array_get_idx(a,i),*hs;if(json_object_object_get_ex(d,"hostnames",&hs)&&json_object_is_type(hs,json_type_array))for(int k=0;k<(int)json_object_array_length(hs);k++)if(!strcasecmp(json_object_get_string(json_object_array_get_idx(hs,k)),host))return 1;}return 0;}
static json_object *discover_devices(void){json_object*out=json_object_new_array();FILE*f=fopen("/tmp/dhcp.leases","r");if(!f)return out;char line[768],mac[32],ip[64],host[128];while(fgets(line,sizeof line,f)){long expiry=0;mac[0]=ip[0]=host[0]=0;if(sscanf(line,"%ld %31s %63s %127s",&expiry,mac,ip,host)<3)continue;if(known_mac(mac))continue;json_object*o=json_object_new_object();json_object_object_add(o,"mac",json_object_new_string(mac));json_object_object_add(o,"ip",json_object_new_string(ip));if(*host&&strcmp(host,"*")&&strcmp(host,"-")){json_object_object_add(o,"hostname",json_object_new_string(host));json_object*h=json_object_new_array();json_object_array_add(h,json_object_new_string(host));json_object_object_add(o,"hostnames",h);}json_object_object_add(o,"lease_expires",json_object_new_int64(expiry));json_object_array_add(out,o);}fclose(f);return out;}
static int device_has_hostname(json_object*d,const char*host){json_object*hs;if(!host||!*host||!strcmp(host,"*")||!strcmp(host,"-"))return 0;if(json_object_object_get_ex(d,"hostnames",&hs)&&json_object_is_type(hs,json_type_array))for(int k=0;k<(int)json_object_array_length(hs);k++)if(!strcasecmp(json_object_get_string(json_object_array_get_idx(hs,k)),host))return 1;return 0;}
static int sync_dhcp_macs(void){FILE*f=fopen("/tmp/dhcp.leases","r");if(!f)return 0;int changed=0,updated[128]={0};char line[768],mac[32],ip[64],host[128];while(fgets(line,sizeof line,f)){long expiry=0;mac[0]=ip[0]=host[0]=0;if(sscanf(line,"%ld %31s %63s %127s",&expiry,mac,ip,host)<4)continue;for(int i=0;i<(int)json_object_array_length(arr())&&i<128;i++){if(updated[i])continue;json_object*d=json_object_array_get_idx(arr(),i);const char*old=sval(d,"mac","");if(device_has_hostname(d,host)&&strcasecmp(old,mac)&&!known_mac(mac)){char oldcopy[32];snprintf(oldcopy,sizeof oldcopy,"%s",old);json_object_object_add(d,"mac",json_object_new_string(mac));int rc=nft_commit();if(rc==0){counters_seen[i]=0;previous_inbound[i]=previous_outbound[i]=0;last_active[i]=0;save();save_state();fprintf(stderr,"parental-control: DHCP update applied device_id=%s old_mac=%s new_mac=%s ip=%s nft_result=success\n",sval(d,"id",""),oldcopy,mac,ip);updated[i]=1;changed=1;}else{json_object_object_add(d,"mac",json_object_new_string(oldcopy));nft_commit();fprintf(stderr,"parental-control: DHCP update rejected device_id=%s old_mac=%s new_mac=%s ip=%s nft_result=failure\n",sval(d,"id",""),oldcopy,mac,ip);}}}}fclose(f);return changed;}
static void history_event(const char *device_id,const char *event){ensure_varlib();json_object*j=json_object_from_file(HISTORY);if(!j||!json_object_is_type(j,json_type_object)){if(j)json_object_put(j);j=json_object_new_object();}json_object*events;if(!json_object_object_get_ex(j,"events",&events)||!json_object_is_type(events,json_type_array)){events=json_object_new_array();json_object_object_add(j,"events",events);}json_object*o=json_object_new_object();json_object_object_add(o,"timestamp",json_object_new_int64(time(NULL)));json_object_object_add(o,"device_id",json_object_new_string(device_id?device_id:""));json_object_object_add(o,"event",json_object_new_string(event?event:""));json_object_array_add(events,o);while(json_object_array_length(events)>1000)json_object_array_del_idx(events,0,1);char tmp[256];snprintf(tmp,sizeof tmp,"%s.tmp",HISTORY);if(json_object_to_file_ext(tmp,j,JSON_C_TO_STRING_PRETTY)==0)rename(tmp,HISTORY);else unlink(tmp);json_object_put(j);}
static json_object *history_read(void){json_object*j=json_object_from_file(HISTORY);if(j)return j;j=json_object_new_object();json_object_object_add(j,"events",json_object_new_array());return j;}
static const char *validate_device(json_object*d,int editing){json_object*v,*hs;if(!d||!json_object_is_type(d,json_type_object))return "invalid device settings";const char*name=sval(d,"name","");const char*mac=sval(d,"mac","");if(!*name||strlen(name)>127||!mac_valid(mac)||!json_object_object_get_ex(d,"hostnames",&hs)||!json_object_is_type(hs,json_type_array)||json_object_array_length(hs)<1||json_object_array_length(hs)>16)return "name, MAC and 1-16 hostnames are required";for(int i=0;i<(int)json_object_array_length(hs);i++){const char*h=json_object_get_string(json_object_array_get_idx(hs,i));if(!hostname_valid(h))return "invalid hostname";for(int k=0;k<i;k++)if(!strcasecmp(h,json_object_get_string(json_object_array_get_idx(hs,k))))return "duplicate hostname";if(!editing&&known_hostname(h))return "duplicate hostname";}int daily=ival(d,"daily_limit_minutes",0),session=ival(d,"session_limit_minutes",0),brk=ival(d,"break_minutes",0),idle=ival(d,"idle_timeout_seconds",300),threshold=ival(d,"activity_threshold_bytes",4096);if(daily<0||daily>1440||session<0||session>1440||brk<0||brk>1440)return "device limits must be between 0 and 1440 minutes";if(bval(d,"daily_limit_enabled",0)&&daily<1)return "daily limit must be at least 1 minute";if(bval(d,"session_limit_enabled",0)&&session<1)return "session limit must be at least 1 minute";if(bval(d,"break_enabled",0)&&brk<1)return "break must be at least 1 minute";if(idle<0||idle>86400)return "idle timeout must be between 0 and 86400 seconds";if(threshold<1||threshold>1073741824)return "activity threshold must be between 1 and 1073741824 bytes";if(bval(d,"night_enabled",0)){json_object*sv=NULL,*ev=NULL;if(!json_object_object_get_ex(d,"night_start",&sv)||!time_valid(json_object_get_string(sv))||!json_object_object_get_ex(d,"night_end",&ev)||!time_valid(json_object_get_string(ev)))return "invalid night time; use 24-hour HH:MM (00:00-23:59)";if(!strcmp(json_object_get_string(sv),json_object_get_string(ev)))return "night start and end must be different";}return NULL;}
static void response(int c,int code,json_object*j){const char*b=json_object_to_json_string_ext(j,JSON_C_TO_STRING_PLAIN);dprintf(c,"HTTP/1.1 %d OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s",code,strlen(b),b);}
static json_object *okmsg(const char*m){json_object*j=json_object_new_object();json_object_object_add(j,"success",json_object_new_boolean(1));if(m)json_object_object_add(j,"message",json_object_new_string(m));return j;}
static void handle(int c){char b[65536];ssize_t n=read_http_request(c,b,sizeof b);if(n==-2){json_object*j=okmsg("request too large");json_object_object_add(j,"success",json_object_new_boolean(0));response(c,413,j);json_object_put(j);return;}if(n<=0)return;b[n]=0;char method[8],path[512];if(sscanf(b,"%7s %511s",method,path)!=2)return;char *body=strstr(b,"\r\n\r\n");body=body?body+4:(char*)"";json_object*j=NULL;int code=200;
if(!strcmp(path,"/api/health")||!strcmp(path,"/api/status")){j=okmsg("ok");json_object_object_add(j,"device_count",json_object_new_int(json_object_array_length(arr())));}
else if(!strcmp(path,"/api/state")){j=state_snapshot();}
else if(!strcmp(path,"/api/devices")&&!strcmp(method,"GET")){j=okmsg(NULL);json_object_object_add(j,"devices",json_object_get(arr()));}
else if(!strcmp(path,"/api/devices")&&!strcmp(method,"POST")){json_object*d=json_tokener_parse(body);const char*err=validate_device(d,0);if(json_object_array_length(arr())>=128){j=okmsg("device limit reached");json_object_object_add(j,"success",json_object_new_boolean(0));code=400;if(d)json_object_put(d);}else if(err){j=okmsg(err);json_object_object_add(j,"success",json_object_new_boolean(0));code=400;if(d)json_object_put(d);}else if(known_mac(sval(d,"mac",""))){j=okmsg("duplicate device id or MAC");json_object_object_add(j,"success",json_object_new_boolean(0));code=409;json_object_put(d);}else{json_object*vdefault=NULL;if(!json_object_object_get_ex(d,"enabled",&vdefault))json_object_object_add(d,"enabled",json_object_new_boolean(1));if(!json_object_object_get_ex(d,"activity_threshold_bytes",&vdefault))json_object_object_add(d,"activity_threshold_bytes",json_object_new_int(4096));if(!json_object_object_get_ex(d,"idle_timeout_seconds",&vdefault))json_object_object_add(d,"idle_timeout_seconds",json_object_new_int(300));json_object_object_del(d,"id");char id[64];make_device_id(sval(d,"mac",""),id);json_object_object_add(d,"id",json_object_new_string(id));if(findid(sval(d,"id",""))>=0){j=okmsg("duplicate device id or MAC");json_object_object_add(j,"success",json_object_new_boolean(0));code=409;json_object_put(d);}else{json_object_array_add(arr(),d);save();nft_dirty=1;nft_commit();save_state();history_event(sval(d,"id",""),"device created");j=okmsg("device created");}}}
else if(!strcmp(path,"/api/discovered")){j=okmsg(NULL);json_object_object_add(j,"devices",discover_devices());}
else if(!strcmp(path,"/api/history")){j=okmsg(NULL);json_object*h=history_read(),*e=NULL;if(json_object_object_get_ex(h,"events",&e))json_object_object_add(j,"events",json_object_get(e));else json_object_object_add(j,"events",json_object_new_array());json_object_put(h);}
else if(!strncmp(path,"/api/devices/",13)){char tmp[512];snprintf(tmp,sizeof tmp,"%s",path+13);char *slash=strchr(tmp,'/');if(slash)*slash++=0;int i=findid(tmp);if(i<0){j=okmsg("device not found");json_object_object_add(j,"success",json_object_new_boolean(0));code=404;}
else if(!slash&&!strcmp(method,"DELETE")){history_event(tmp,"device deleted");json_object_array_del_idx(arr(),i,1);runtime_remove(i);save();nft_dirty=1;nft_commit();save_state();j=okmsg("device deleted");}
else if(!slash&&!strcmp(method,"PUT")){json_object*d=json_tokener_parse(body);const char*err=validate_device(d,1);int duplicate=0;if(d){const char*newmac=sval(d,"mac","");for(int k=0;k<(int)json_object_array_length(arr());k++)if(k!=i&&!strcasecmp(sval(json_object_array_get_idx(arr(),k),"mac",""),newmac))duplicate=1;json_object*hs=NULL;if(json_object_object_get_ex(d,"hostnames",&hs)&&json_object_is_type(hs,json_type_array))for(int h=0;h<(int)json_object_array_length(hs);h++){const char*host=json_object_get_string(json_object_array_get_idx(hs,h));for(int k=0;k<(int)json_object_array_length(arr());k++)if(k!=i&&device_has_hostname(json_object_array_get_idx(arr(),k),host))duplicate=1;}}if(err){j=okmsg(err);json_object_object_add(j,"success",json_object_new_boolean(0));code=400;if(d)json_object_put(d);}else if(duplicate){j=okmsg("duplicate hostname or MAC");json_object_object_add(j,"success",json_object_new_boolean(0));code=409;json_object_put(d);}else{json_object *old=json_object_array_get_idx(arr(),i),*v=NULL,*present=NULL;int mac_changed=strcasecmp(sval(old,"mac",""),sval(d,"mac",""))!=0;if(json_object_object_get_ex(old,"id",&v))json_object_object_add(d,"id",json_object_get(v));const char*preserve[]={"enabled","activity_threshold_bytes","idle_timeout_seconds"};for(size_t k=0;k<sizeof(preserve)/sizeof(preserve[0]);k++){present=NULL;if(!json_object_object_get_ex(d,preserve[k],&present)&&json_object_object_get_ex(old,preserve[k],&v))json_object_object_add(d,preserve[k],json_object_get(v));}json_object_array_put_idx(arr(),i,d);if(mac_changed){counters_seen[i]=0;previous_inbound[i]=previous_outbound[i]=0;last_active[i]=0;}if(!bval(d,"session_limit_enabled",0)||!bval(d,"break_enabled",0))session_seconds[i]=0;if(!bval(d,"break_enabled",0)){break_active[i]=0;brk_until[i]=0;}else if(bval(d,"session_limit_enabled",0)){int lim=ival(d,"session_limit_minutes",0)*60;if(lim>0&&session_seconds[i]>=lim&&!break_active[i]){break_active[i]=1;brk_until[i]=time(NULL)+ival(d,"break_minutes",0)*60;session_seconds[i]=0;}}save();nft_dirty=1;nft_commit();save_state();history_event(tmp,"device updated");j=okmsg("device updated");}}
else if(slash&&!strcmp(method,"POST")){const char*ev="action applied";if(!strcmp(slash,"block")||!strcmp(slash,"unblock")){int old_manual=manual[i],old_temp=temporary_unblock[i];int requested=!strcmp(slash,"block");manual[i]=requested;temporary_unblock[i]=requested?0:1;counters_seen[i]=0;previous_inbound[i]=previous_outbound[i]=0;last_active[i]=0;nft_dirty=1;if(nft_commit()!=0){manual[i]=old_manual;temporary_unblock[i]=old_temp;nft_dirty=1;nft_commit();j=okmsg("nftables rule was not applied");json_object_object_add(j,"success",json_object_new_boolean(0));json_object_object_add(j,"nft_exit_status",json_object_new_int(1));code=500;response(c,code,j);json_object_put(j);return;}ev=requested?"blocked":"unblocked";}else if(!strcmp(slash,"break")){if(!bval(json_object_array_get_idx(arr(),i),"break_enabled",0)){j=okmsg("break is disabled");json_object_object_add(j,"success",json_object_new_boolean(0));code=400;response(c,code,j);json_object_put(j);return;}break_active[i]=1;brk_until[i]=time(NULL)+ival(json_object_array_get_idx(arr(),i),"break_minutes",60)*60;ev="break started";}else if(!strcmp(slash,"bonus")){json_object*q=json_tokener_parse(body);int mins=q?ival(q,"minutes",0):0;if(q)json_object_put(q);if(mins<=0||mins>1440||bonus_minutes[i]+mins>1440){j=okmsg("invalid bonus");json_object_object_add(j,"success",json_object_new_boolean(0));response(c,400,j);json_object_put(j);return;}bonus_minutes[i]+=mins;ev="bonus added";}else if(!strcmp(slash,"reset-today")){used_seconds[i]=session_seconds[i]=bonus_minutes[i]=0;break_active[i]=0;brk_until[i]=0;temporary_unblock[i]=0;ev="today reset";}else if(!strcmp(slash,"reset_temporary_state")){temporary_unblock[i]=0;ev="temporary state reset";}else{j=okmsg("route not found");json_object_object_add(j,"success",json_object_new_boolean(0));code=404;response(c,code,j);json_object_put(j);return;}if(strcmp(slash,"block")&&strcmp(slash,"unblock")){nft_dirty=1;nft_commit();}save_state();history_event(tmp,ev);j=okmsg(ev);}else{j=okmsg("route not found");json_object_object_add(j,"success",json_object_new_boolean(0));code=404;}}
else{j=okmsg("route not found");json_object_object_add(j,"success",json_object_new_boolean(0));code=404;}response(c,code,j);json_object_put(j);}
int main(void){signal(SIGPIPE,SIG_IGN);root=json_object_from_file(DEVICES);if(!root){if(access(DEVICES,F_OK)==0){fprintf(stderr,"parental-control: failed to parse %s; refusing to replace it with an empty config\n",DEVICES);return 1;}root=json_object_new_object();json_object_object_add(root,"devices",json_object_new_array());save();}json_object*startup_devices=NULL;if(!json_object_is_type(root,json_type_object)||!json_object_object_get_ex(root,"devices",&startup_devices)||!json_object_is_type(startup_devices,json_type_array)||json_object_array_length(startup_devices)>128){fprintf(stderr,"parental-control: invalid %s: expected object with devices array (max 128)\n",DEVICES);return 1;}for(int i=0;i<(int)json_object_array_length(startup_devices);i++){json_object*d=json_object_array_get_idx(startup_devices,i);const char*id=sval(d,"id","");const char*err=validate_device(d,1);if(!id_valid(id)||err){fprintf(stderr,"parental-control: invalid device %d in %s: %s\n",i,DEVICES,!id_valid(id)?"invalid id":err);return 1;}for(int k=0;k<i;k++){json_object*other=json_object_array_get_idx(startup_devices,k);if(!strcmp(id,sval(other,"id",""))||!strcasecmp(sval(d,"mac",""),sval(other,"mac",""))){fprintf(stderr,"parental-control: duplicate device id or MAC in %s\n",DEVICES);return 1;}json_object*hs=NULL;if(json_object_object_get_ex(d,"hostnames",&hs)&&json_object_is_type(hs,json_type_array))for(int h=0;h<(int)json_object_array_length(hs);h++)if(device_has_hostname(other,json_object_get_string(json_object_array_get_idx(hs,h)))){fprintf(stderr,"parental-control: duplicate hostname in %s\n",DEVICES);return 1;}}}unlink(SOCK);int s=socket(AF_UNIX,SOCK_STREAM,0);struct sockaddr_un a={.sun_family=AF_UNIX};strncpy(a.sun_path,SOCK,sizeof(a.sun_path)-1);mkdir("/var/run",0755);if(bind(s,(struct sockaddr*)&a,sizeof a)||listen(s,32)){perror("parental-control");return 1;}chmod(SOCK,0660);load_state();save_state();nft_dirty=1;if(nft_commit()!=0){fprintf(stderr,"parental-control: initial nftables apply failed\n");close(s);unlink(SOCK);return 1;}for(;;){struct pollfd pfd={.fd=s,.events=POLLIN};int r=poll(&pfd,1,1000);tick();if(r>0&&(pfd.revents&POLLIN)){int c=accept(s,NULL,NULL);if(c>=0){handle(c);close(c);}}}}
