// main.c - IPMI TUI (btop++风格) for Linux, pure C + ncurses
#include <ncurses.h>
#include <time.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <locale.h>
#include <ctype.h>

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
    // 阴影效果
    attron(A_DIM);
    for(int i=1; i<h; ++i) mvprintw(y+i, x+w, " ");
    for(int i=2; i<=w; ++i) mvprintw(y+h, x+i, " ");
    attroff(A_DIM);

    // 彩色圆角box
    attron(COLOR_PAIR(1));
    mvprintw(y, x, "╭");
    for(int i=1; i<w-1; ++i) mvprintw(y, x+i, "─");
    mvprintw(y, x+w-1, "╮");
    for(int i=1; i<h-1; ++i) {
        mvprintw(y+i, x, "│");
        mvprintw(y+i, x+w-1, "│");
    }
    mvprintw(y+h-1, x, "╰");
    for(int i=1; i<w-1; ++i) mvprintw(y+h-1, x+i, "─");
    mvprintw(y+h-1, x+w-1, "╯");
    if(title && strlen(title)>0) {
        attron(A_BOLD | COLOR_PAIR(2));
        mvprintw(y, x+2, " %s ", title);
        attroff(A_BOLD | COLOR_PAIR(2));
    }
    attroff(COLOR_PAIR(1));
}

void draw_separator(int y, int x, int w, const char *title) {
    attron(COLOR_PAIR(1));
    mvprintw(y, x, "├");
    for(int i=1; i<w-1; ++i) mvprintw(y, x+i, "─");
    mvprintw(y, x+w-1, "┤");
    if(title && strlen(title)>0) {
        attron(A_BOLD | COLOR_PAIR(2));
        mvprintw(y, x+2, " %s ", title);
        attroff(A_BOLD | COLOR_PAIR(2));
    }
    attroff(COLOR_PAIR(1));
}

void draw_sensor_chart(int y, int x, int w, struct Sensor *s) {
    float mx=0, mn=10000;
    for(int i=0;i<HISTORY_LEN;++i) {
        if(s->history[i]>mx) mx=s->history[i];
        if(s->history[i]<mn) mn=s->history[i];
    }
    if(mx==mn) mx+=1;
    int flash = (time(NULL)%2==0); // 闪烁动画
    const char* blocks[] = {" ", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
    for(int i=0;i<w;++i) {
        int idx = (s->hist_idx+i)%HISTORY_LEN;
        float v = s->history[idx];
        int level = (int)((v-mn)/(mx-mn)*7);
        if(level < 0) level = 0;
        if(level > 7) level = 7;
        
        // 渐变色：低蓝，高红，中绿
        if(v < mn + (mx-mn)*0.33) attron(COLOR_PAIR(6));
        else if(v > mn + (mx-mn)*0.66) attron(COLOR_PAIR(4));
        else attron(COLOR_PAIR(3));
        
        // 温度变化大时闪烁
        if(level>=6 && flash) attron(A_BLINK);
        mvprintw(y, x+i, "%s", blocks[level]);
        attroff(A_BLINK);
        attroff(COLOR_PAIR(3)); attroff(COLOR_PAIR(4)); attroff(COLOR_PAIR(6));
    }
    // 动画符号与当前值
    static const char *anim[] = {"|", "/", "-", "\\"};
    int frame = (time(NULL)%4);
    int last_idx = (s->hist_idx - 1 + HISTORY_LEN) % HISTORY_LEN;
    attron(A_BOLD);
    mvprintw(y, x + w + 1, "%5.1f %s", s->history[last_idx], anim[frame]);
    attroff(A_BOLD);
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

/* 根据配置构建 ipmitool 命令前缀，OOB 模式加上 -I lanplus 参数，
   并将 stderr 重定向到 /dev/null 防止错误信息污染 TUI 界面 */
static void build_cmd(char *buf, size_t bufsz,
                      const struct AppConfig *cfg, const char *subcmd) {
    if(strcmp(cfg->mode, "oob") == 0) {
        snprintf(buf, bufsz,
            "ipmitool -I lanplus -H '%s' -U '%s' -P '%s' %s 2>/dev/null",
            cfg->host, cfg->username, cfg->password, subcmd);
    } else {
        /* inband: 优先尝试 /dev/ipmi0，找不到时 ipmitool 自动 fallback */
        snprintf(buf, bufsz, "ipmitool %s 2>/dev/null", subcmd);
    }
}

void fetch_ipmi(struct AppState *state) {
    state->num_temps=0; state->num_fans=0;
    state->power[0]=0; state->bmc[0]=0; state->last_error[0]=0;

    char cmd[512];

    /* OOB 模式下若 host 为空，直接报错跳过 */
    if(strcmp(state->config.mode, "oob") == 0 && state->config.host[0] == '\0') {
        snprintf(state->last_error, sizeof(state->last_error),
                 "OOB mode: host not configured");
        return;
    }

    // chassis power
    build_cmd(cmd, sizeof(cmd), &state->config, "chassis power status");
    FILE *fp = popen(cmd, "r");
    if(fp) {
        if(!fgets(state->power, sizeof(state->power), fp))
            state->power[0] = '\0';
        state->power[strcspn(state->power,"\r\n")] = 0;
        int rc = pclose(fp);
        if(rc != 0 && state->power[0] == '\0')
            snprintf(state->last_error, sizeof(state->last_error),
                     "ipmitool error (power), rc=%d", rc);
    }

    // mc info
    build_cmd(cmd, sizeof(cmd), &state->config, "mc info");
    fp = popen(cmd, "r");
    if(fp) {
        char line[128];
        while(fgets(line,sizeof(line),fp)) {
            if(strstr(line,"Firmware Revision")) {
                char *p = strchr(line, ':');
                if(p) { strcpy(state->bmc, p+2); }
                state->bmc[strcspn(state->bmc,"\r\n")] = 0;
            }
        }
        pclose(fp);
    }

    // sensor
    build_cmd(cmd, sizeof(cmd), &state->config, "sensor");
    fp = popen(cmd, "r");
    if(fp) {
        char line[256];
        while(fgets(line,sizeof(line),fp)) {
            if(!strstr(line,"|")) continue;
            /* 复制一份用于 strtok，保留原行做关键字匹配 */
            char linecopy[256];
            strncpy(linecopy, line, sizeof(linecopy)-1);
            linecopy[sizeof(linecopy)-1] = '\0';
            char *name = strtok(linecopy, "|");
            char *val  = strtok(NULL, "|");
            char *unit = strtok(NULL, "|");
            if(!name || !val || !unit) continue;
            /* 去除首尾空格 */
            while(*name==' ') name++;
            while(*val ==' ') val++;
            while(*unit==' ') unit++;
            char *e;
            if((e=strchr(name,' '))) *e='\0';
            if((e=strchr(val ,' '))) *e='\0';
            if((e=strchr(unit,' '))) *e='\0';

            float v = atof(val);
            /* 过滤无效值（ipmitool 用 "na" 表示无效传感器） */
            if(val[0]=='n' || val[0]=='N') continue;

            /* 温度传感器 */
            char line_lower[256];
            strncpy(line_lower, line, sizeof(line_lower)-1);
            for(int k=0; line_lower[k]; k++) line_lower[k]=(char)tolower((unsigned char)line_lower[k]);

            if((strstr(line_lower,"temp") || strstr(name,"Temp"))
               && state->num_temps < MAX_SENSORS) {
                struct Sensor *s = &state->temps[state->num_temps];
                strncpy(s->name, name, 63); s->name[63]='\0';
                s->history[s->hist_idx] = v;
                s->hist_idx = (s->hist_idx+1) % HISTORY_LEN;
                state->num_temps++;
            }
            /* 风扇传感器 */
            if((strstr(line_lower,"fan") || strstr(name,"Fan"))
               && state->num_fans < MAX_SENSORS) {
                struct Sensor *s = &state->fans[state->num_fans];
                strncpy(s->name, name, 63); s->name[63]='\0';
                s->history[s->hist_idx] = v;
                s->hist_idx = (s->hist_idx+1) % HISTORY_LEN;
                state->num_fans++;
            }
        }
        pclose(fp);
    }

    // sel list (最新 5 条)
    build_cmd(cmd, sizeof(cmd), &state->config, "sel list last 5");
    fp = popen(cmd, "r");
    if(fp) {
        int i = 0; char line[128];
        while(fgets(line,sizeof(line),fp) && i<5) {
            strncpy(state->sel[i], line, 127);
            state->sel[i][strcspn(state->sel[i],"\r\n")] = 0;
            i++;
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
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);
    
    int left_w = 36;
    int right_w = max_x - left_w - 1;
    if(right_w < 30) right_w = 30; // 最小宽度
    
    int top_h = max_y - 2; // 留给状态栏
    int temp_h = top_h / 2;
    int fan_h = top_h - temp_h;
    
    draw_box(0,0,top_h,left_w,"Device Info");
    
    attron(A_BOLD | COLOR_PAIR(2));
    mvprintw(2,2,"Power:");
    mvprintw(3,2,"BMC FW:");
    attroff(A_BOLD | COLOR_PAIR(2));
    mvprintw(2,9,"%s", state->power);
    mvprintw(3,10,"%s", state->bmc);
    
    draw_separator(4, 0, left_w, "Configuration");
    
    attron(A_BOLD | COLOR_PAIR(1));
    mvprintw(5,2,"Mode: %s",state->config.mode);
    mvprintw(6,2,"Host: %s",state->config.host);
    mvprintw(7,2,"User: %s",state->config.username);
    attroff(A_BOLD | COLOR_PAIR(1));
    
    attron(A_BOLD | COLOR_PAIR(3));
    mvprintw(8,2,"Refresh: %d s",state->config.refresh_interval);
    mvprintw(9,2,"Remember: %s",state->config.remember_cred ? "yes" : "no");
    attroff(A_BOLD | COLOR_PAIR(3));
    
    draw_separator(11, 0, left_w, "System Event Log");
    
    attron(A_BOLD | COLOR_PAIR(4));
    for(int i=0;i<5 && 12+i < top_h-1; ++i) mvprintw(12+i,2,"%.*s", left_w-4, state->sel[i]);
    attroff(A_BOLD | COLOR_PAIR(4));

    draw_box(0, left_w, temp_h, right_w, "Temperature Sensors");
    for(int i=0;i<state->num_temps && 2+i < temp_h-1;++i) {
        attron(COLOR_PAIR(2) | A_BOLD);
        mvprintw(2+i, left_w+2, "%-7.7s",state->temps[i].name);
        attroff(COLOR_PAIR(2) | A_BOLD);
        int chart_w = right_w - 20;
        if(chart_w > 0) draw_sensor_chart(2+i, left_w+10, chart_w, &state->temps[i]);
    }
    
    draw_box(temp_h, left_w, fan_h, right_w, "Fan Sensors");
    for(int i=0;i<state->num_fans && temp_h+1+i < max_y-2;++i) {
        attron(COLOR_PAIR(1) | A_BOLD);
        mvprintw(temp_h+1+i, left_w+2, "%-7.7s",state->fans[i].name);
        attroff(COLOR_PAIR(1) | A_BOLD);
        int chart_w = right_w - 20;
        if(chart_w > 0) draw_sensor_chart(temp_h+1+i, left_w+10, chart_w, &state->fans[i]);
    }
    
    // 状态栏
    attron(COLOR_PAIR(5) | A_BOLD);
    for(int i=0; i<max_x; i++) mvprintw(max_y-1, i, " ");
    mvprintw(max_y-1,1," [C] Config   [Q] Quit ");
    if (strlen(state->last_error) > 0) {
        attron(COLOR_PAIR(4) | A_REVERSE | A_BLINK);
        mvprintw(max_y-1,30," Error: %s ",state->last_error);
        attroff(COLOR_PAIR(4) | A_REVERSE | A_BLINK);
    }
    mvprintw(max_y-1,max_x-40," Mode:%s Rfrsh:%ds ",state->config.mode,state->config.refresh_interval);
    attroff(COLOR_PAIR(5) | A_BOLD);
    
    refresh();
}

int main() {
    setlocale(LC_ALL, "");
    struct AppState state;
    load_config(&state.config);
    initscr(); cbreak(); noecho(); keypad(stdscr,TRUE);
    timeout(500); // 500ms 不阻塞，用于处理调整大小和定时刷新
    signal(SIGINT,SIG_IGN);
    if(has_colors()) {
        start_color();
        use_default_colors();
        init_pair(1, COLOR_CYAN, -1);      // box title - 亮色
        init_pair(2, COLOR_YELLOW, -1);    // sensor name
        init_pair(3, COLOR_GREEN, -1);     // normal value
        init_pair(4, COLOR_RED, -1);       // high temp/fan
        init_pair(5, COLOR_WHITE, COLOR_BLUE);   // status bar (亮色背景)
        init_pair(6, COLOR_BLUE, -1);      // low temp/fan
        // 尝试使用高亮颜色 (Bright colors)
        init_pair(7, COLOR_MAGENTA, -1);
    }
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
