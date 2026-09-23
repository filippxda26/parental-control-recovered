#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define SOCK_PATH "/var/run/parental-control.sock"
#define WWW "/usr/share/parental-control/www"
static int sendall(int fd,const void *p,size_t n){const char *s=p;while(n){ssize_t w=write(fd,s,n);if(w<=0)return -1;s+=w;n-=w;}return 0;}
static void reply(int fd,int code,const char *ct,const void *body,size_t n){char h[512];int m=snprintf(h,sizeof h,"HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",code,code==200?"OK":code==404?"Not Found":"Bad Request",ct,n);sendall(fd,h,m);sendall(fd,body,n);}
static const char *mime(const char *p){size_t n=strlen(p);if(n>4&&!strcmp(p+n-4,".css"))return "text/css";if(n>3&&!strcmp(p+n-3,".js"))return "application/javascript";if(n>5&&!strcmp(p+n-5,".json"))return "application/manifest+json";if(n>4&&!strcmp(p+n-4,".svg"))return "image/svg+xml";return "text/html; charset=utf-8";}
static void static_file(int c,const char *path){const char *name=!strcmp(path,"/")?"index.html":path+1;if(strstr(name,"..")){reply(c,400,"application/json","{\"success\":false}",17);return;}char f[512];snprintf(f,sizeof f,"%s/%s",WWW,name);FILE *fp=fopen(f,"rb");if(!fp){reply(c,404,"application/json","{\"success\":false}",17);return;}fseek(fp,0,SEEK_END);long n=ftell(fp);rewind(fp);char *b=malloc(n?n:1);if(b&&fread(b,1,n,fp)==(size_t)n)reply(c,200,mime(f),b,n);free(b);fclose(fp);}
static void proxy_api(int c,const char *req,size_t n){int s=socket(AF_UNIX,SOCK_STREAM,0);struct sockaddr_un a={.sun_family=AF_UNIX};strncpy(a.sun_path,SOCK_PATH,sizeof(a.sun_path)-1);if(s<0||connect(s,(struct sockaddr*)&a,sizeof a)<0){const char *e="{\"success\":false,\"message\":\"service unavailable\"}";reply(c,503,"application/json",e,strlen(e));if(s>=0)close(s);return;}sendall(s,req,n);char b[8192];ssize_t r;while((r=read(s,b,sizeof b))>0)sendall(c,b,r);close(s);}
static void handle(int c){char b[65536];ssize_t n=read(c,b,sizeof(b)-1);if(n<=0)return;b[n]=0;char method[8],path[256];if(sscanf(b,"%7s %255s",method,path)!=2){reply(c,400,"application/json","{\"success\":false}",17);return;}char *cl=strcasestr(b,"Content-Length:");long need=cl?strtol(cl+15,NULL,10):0;char *end=strstr(b,"\r\n\r\n");size_t have=end?(size_t)(b+n-(end+4)):0;while(end&&have<(size_t)need&&n<(ssize_t)sizeof(b)-1){ssize_t r=read(c,b+n,sizeof(b)-1-n);if(r<=0)break;n+=r;b[n]=0;have+=r;}if(!strncmp(path,"/api/",5))proxy_api(c,b,n);else static_file(c,path);}
int main(int argc,char **argv){signal(SIGPIPE,SIG_IGN);const char *bindip=argc>1?argv[1]:"0.0.0.0";int port=5000,s=socket(AF_INET,SOCK_STREAM,0),one=1;setsockopt(s,SOL_SOCKET,SO_REUSEADDR,&one,sizeof one);struct sockaddr_in a={.sin_family=AF_INET,.sin_port=htons(port)};if(inet_pton(AF_INET,bindip,&a.sin_addr)!=1||bind(s,(struct sockaddr*)&a,sizeof a)||listen(s,32)){perror("parental-control-web");return 1;}for(;;){int c=accept(s,NULL,NULL);if(c>=0){handle(c);close(c);}}}
