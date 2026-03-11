// main.c - IPMI TUI (btop++风格) for Linux, pure C + ncurses
#include <ncurses.h>
#include <time.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

#define CONFIG_PATH "./.config/ipmi_tui/config.ini"
#define MAX_SENSORS 16
#define HISTORY_LEN 40

struct Sensor {
    char name[64];
    float history[HISTORY_LEN];
    int hist_idx;
};

struct AppConfig {
    char mode[16];
    char host[64];
    char username[32];
    char password[32];
    int refresh_interval;
    int remember_cred;
};

struct AppState {
    struct AppConfig config;
    struct Sensor temps[MAX_SENSORS];
    int num_temps;
    struct Sensor fans[MAX_SENSORS];
    int num_fans;
    char power[32];
    char bmc[32];
    char sel[5][128];
    char last_error[128];
};

void draw_box(int y, int x, int h, int w, const char *title) {
    mvprintw(y, x, "+%.*s+", w-2, "----------------------------------------");
    for(int i=1; i<h-1; ++i) {
        mvprintw(y+i, x, "|");
        mvprintw(y+i, x+w-1, "|");
    }
    mvprintw(y+h-1, x, "+%.*s+", w-2, "----------------------------------------");
    if(title && strlen(title)>0) {
        mvprintw(y, x+2, " %s ", title);
    }
}

void draw_sensor_chart(int y, int x, int w, struct Sensor *s) {
    float mx=0, mn=10000;
    for(int i=0;i<HISTORY_LEN;++i) {
        if(s->history[i]>mx) mx=s->history[i];
        if(s->history[i]<mn) mn=s->history[i];
    }
    if(mx==mn) mx+=1;
    for(int i=0;i<w;++i) {
        int idx = (s->hist_idx+i)%HISTORY_LEN;
        float v = s->history[idx];
        int level = (int)((v-mn)/(mx-mn)*7);
        char blocks[] = " ▂▃▄▅▆▇█";
        mvaddch(y, x+i, blocks[level]);
    }
    mvprintw(y+1, x, "Min:%.1f Max:%.1f", mn, mx);
}

void load_config(struct AppConfig *cfg) {
    FILE *f = fopen(CONFIG_PATH, "r");
    if(!f) { // defaults
        strcpy(cfg->mode, "inband");
        cfg->host[0]=0; cfg->username[0]=0; cfg->password[0]=0;
        cfg->refresh_interval=3; cfg->remember_cred=0;
        return;
    }
    char line[128];
    while(fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '=');
        if(!eq) continue;
        *eq=0; char *key=line, *val=eq+1;
        val[strcspn(val,"\r\n")]=0;
        if(strcmp(key,"mode")==0) strcpy(cfg->mode,val);
        else if(strcmp(key,"host")==0) strcpy(cfg->host,val);
        else if(strcmp(key,"username")==0) strcpy(cfg->username,val);
        else if(strcmp(key,"password")==0) strcpy(cfg->password,val);
        else if(strcmp(key,"refresh_interval")==0) cfg->refresh_interval=atoi(val);
        else if(strcmp(key,"remember_cred")==0) cfg->remember_cred=atoi(val);
    }
    fclose(f);
}

void save_config(struct AppConfig *cfg) {
    FILE *f = fopen(CONFIG_PATH, "w");
    if(!f) return;
    fprintf(f, "mode=%s\nhost=%s\nusername=%s\npassword=%s\nrefresh_interval=%d\nremember_cred=%d\n",
        cfg->mode, cfg->host, cfg->username, cfg->password, cfg->refresh_interval, cfg->remember_cred);
    fclose(f);
}

void fetch_ipmi(struct AppState *state) {
    // 清空
    state->num_temps=0; state->num_fans=0; state->power[0]=0; state->bmc[0]=0; state->last_error[0]=0;
    // chassis power
    FILE *fp = popen("ipmitool chassis power status", "r");
    if(fp) { fgets(state->power,32,fp); state->power[strcspn(state->power,"\r\n")]=0; pclose(fp); }
    // mc info
    fp = popen("ipmitool mc info", "r");
    if(fp) {
        char line[128];
        while(fgets(line,128,fp)) {
            if(strstr(line,"Firmware Revision")) {
                char *p=strchr(line,':');
                if(p) strcpy(state->bmc,p+1);
                state->bmc[strcspn(state->bmc,"\r\n")]=0;
            }
        }
        pclose(fp);
    }
    // sensor
    fp = popen("ipmitool sensor", "r");
    if(fp) {
        char line[128];
        while(fgets(line,128,fp)) {
            if(strstr(line,"|")) {
                char *name=strtok(line,"|");
                char *val=strtok(NULL,"|");
                if(!name||!val) continue;
                name=strtok(name," ");
                val=strtok(val," ");
                if(strstr(line,"temp") && (strstr(line,"celsius")||strstr(line,"degrees c"))) {
                    struct Sensor *s = &state->temps[state->num_temps++];
                    strncpy(s->name,name,63); s->hist_idx=0;
                    float v=atof(val); s->history[s->hist_idx++]=v;
                }
                if(strstr(line,"fan") && strstr(line,"rpm")) {
                    struct Sensor *s = &state->fans[state->num_fans++];
                    strncpy(s->name,name,63); s->hist_idx=0;
                    float v=atof(val); s->history[s->hist_idx++]=v;
                }
            }
        }
        pclose(fp);
    }
    // sel
    fp = popen("ipmitool sel list", "r");
    if(fp) {
        int i=0; char line[128];
        while(fgets(line,128,fp)&&i<5) {
            strncpy(state->sel[i++],line,127);
            state->sel[i-1][strcspn(state->sel[i-1],"\r\n")]=0;
        }
        pclose(fp);
    }
}

void draw_config_editor(struct AppConfig *cfg) {
    clear();
    draw_box(1,2,12,60,"Edit Config");
    mvprintw(3,4,"Mode (inband/oob): %s",cfg->mode);
    mvprintw(4,4,"Host: %s",cfg->host);
    mvprintw(5,4,"Username: %s",cfg->username);
    mvprintw(6,4,"Password: %s",cfg->password);
    mvprintw(7,4,"Refresh Interval: %d",cfg->refresh_interval);
    mvprintw(8,4,"Remember Creds: %d",cfg->remember_cred);
    mvprintw(10,4,"[E]dit field  [S]ave  [Q]uit config");
    refresh();
}

void edit_config(struct AppConfig *cfg) {
    int ch;
    while(1) {
        draw_config_editor(cfg);
        ch = getch();
        if(ch=='q'||ch=='Q') break;
        if(ch=='s'||ch=='S') { save_config(cfg); break; }
        if(ch=='e'||ch=='E') {
            mvprintw(12,4,"Field (1-mode 2-host 3-user 4-pass 5-interval 6-remember): ");
            refresh();
            int f = getch()-'0';
            char buf[64];
            echo();
            switch(f) {
                case 1: mvprintw(13,4,"New mode: "); getnstr(buf,15); strcpy(cfg->mode,buf); break;
                case 2: mvprintw(13,4,"New host: "); getnstr(buf,63); strcpy(cfg->host,buf); break;
                case 3: mvprintw(13,4,"New username: "); getnstr(buf,31); strcpy(cfg->username,buf); break;
                case 4: mvprintw(13,4,"New password: "); getnstr(buf,31); strcpy(cfg->password,buf); break;
                case 5: mvprintw(13,4,"New interval: "); getnstr(buf,7); cfg->refresh_interval=atoi(buf); break;
                case 6: mvprintw(13,4,"Remember creds (0/1): "); getnstr(buf,3); cfg->remember_cred=atoi(buf); break;
            }
            noecho();
        }
    }
}

void draw_main(struct AppState *state) {
    clear();
    draw_box(0,0,20,36,"Device Info");
    mvprintw(2,2,"Power: %s",state->power);
    mvprintw(3,2,"BMC FW: %s",state->bmc);
    mvprintw(4,2,"Mode: %s",state->config.mode);
    mvprintw(5,2,"Host: %s",state->config.host);
    mvprintw(6,2,"User: %s",state->config.username);
    mvprintw(7,2,"Refresh: %d",state->config.refresh_interval);
    mvprintw(8,2,"Remember: %d",state->config.remember_cred);
    mvprintw(10,2,"SEL:");
    for(int i=0;i<5;++i) mvprintw(11+i,2,"%s",state->sel[i]);
    draw_box(0,36,10,44,"Temperature Sensors");
    for(int i=0;i<state->num_temps;++i) {
        mvprintw(2+i,38,"%s",state->temps[i].name);
        draw_sensor_chart(3+i,46,30,&state->temps[i]);
    }
    draw_box(10,36,10,44,"Fan Sensors");
    for(int i=0;i<state->num_fans;++i) {
        mvprintw(12+i,38,"%s",state->fans[i].name);
        draw_sensor_chart(13+i,46,30,&state->fans[i]);
    }
    mvprintw(19,2,"[C]onfig  [Q]uit  LastError: %s",state->last_error);
    refresh();
}

int main() {
    struct AppState state;
    load_config(&state.config);
    initscr(); cbreak(); noecho(); keypad(stdscr,TRUE);
    signal(SIGINT,SIG_IGN);
    int ch, last_refresh=0;
    while(1) {
        if(time(NULL)-last_refresh>=state.config.refresh_interval) {
            fetch_ipmi(&state); last_refresh=time(NULL);
        }
        draw_main(&state);
        ch = getch();
        if(ch=='q'||ch=='Q') break;
        if(ch=='c'||ch=='C') edit_config(&state.config);
    }
    endwin();
    return 0;
}
