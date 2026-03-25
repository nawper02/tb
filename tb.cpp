#include "td_app.h"
#include "pb_app.h"

#include <memory>
#include <vector>

class Shell {
    std::vector<std::unique_ptr<AppBase>> apps;
    int active = 0;

    void draw_tabs() {
        attron(COLOR_PAIR(CP_TAB_INACTIVE) | A_DIM);
        move(0, 0);
        for (int i = 0; i < COLS; ++i) addch(' ');
        attroff(COLOR_PAIR(CP_TAB_INACTIVE) | A_DIM);

        int x = 1;
        for (int i = 0; i < (int)apps.size(); ++i) {
            move(0, x);
            if (i == active) {
                attron(COLOR_PAIR(CP_TAB_ACTIVE) | A_BOLD);
                addstr(" ");
                addstr(apps[i]->id());
                addstr(" ");
                attroff(COLOR_PAIR(CP_TAB_ACTIVE) | A_BOLD);
            } else {
                attron(COLOR_PAIR(CP_TAB_INACTIVE) | A_DIM);
                addstr(" ");
                addstr(apps[i]->id());
                addstr(" ");
                attroff(COLOR_PAIR(CP_TAB_INACTIVE) | A_DIM);
            }
            x += (int)strlen(apps[i]->id()) + 2;
            if (i < (int)apps.size() - 1) {
                attron(COLOR_PAIR(CP_TAB_INACTIVE) | A_DIM);
                addch('|');
                attroff(COLOR_PAIR(CP_TAB_INACTIVE) | A_DIM);
                ++x;
            }
        }

        // Show switch hint on the right
        const char* hint = "Shift+</>:switch";
        int hint_len = (int)strlen(hint);
        int rx = COLS - hint_len - 1;
        if (rx > x + 2) {
            move(0, rx);
            attron(COLOR_PAIR(CP_TAB_INACTIVE) | A_DIM);
            addstr(hint);
            attroff(COLOR_PAIR(CP_TAB_INACTIVE) | A_DIM);
        }
    }

public:
    void run() {
        // Register apps
        apps.push_back(std::make_unique<TdApp>());
        apps.push_back(std::make_unique<PasteApp>());

        // Init ncurses
        initscr();
        raw();
        noecho();
        keypad(stdscr, TRUE);
        curs_set(0);
        set_escdelay(25);
        timeout(50);
        if (has_colors()) init_colors();

        // Set top_y for all apps (row 1, below tab bar)
        for (auto& app : apps) {
            app->top_y = 1;
            app->init();
        }

        // Main loop
        while (true) {
            erase();
            apps[active]->draw();
            draw_tabs();
            refresh();

            int ch = getch();
            if (ch == ERR) continue;

            // Shell-level: Shift+Left/Right to switch tabs
            if (ch == KEY_SLEFT) {
                if (active > 0) --active;
                else active = (int)apps.size() - 1;
                continue;
            }
            if (ch == KEY_SRIGHT) {
                if (active < (int)apps.size() - 1) ++active;
                else active = 0;
                continue;
            }

            // Pass to active app
            if (!apps[active]->handle(ch)) break;
        }

        endwin();
    }
};

int main() {
    Shell shell;
    shell.run();
    return 0;
}
