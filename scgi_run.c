/*
 Execute a CGI script over a SCGI connection

 Compile and strip with:

    cc -o scgi_run scgi_run.c
    strip scgi_run

 For optional debugging, define a DEBUG_FILENAME macro as in:

    cc -o scgi_run -DDEBUG_FILENAME=/tmp/foo.log scgi_run.c

 2011-03-30 Barry Pederson <bp@barryp.org>

*/
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

extern char **environ;
extern int errno;

#define MAX_HEADER_LENGTH 262144  // just for a sanity check on what the server sends

/**
 Debugging code, requires stdarg.h, stdio.h, and time.h

**/
#ifdef DEBUG_FILENAME
    #define QUOTE(name) #name
    #define STR(macro) QUOTE(macro)

    static void debug_log(const char *msg, ...) {
        char timestamp[32]; // really only need 21 bytes here
        time_t now;
        va_list ap;
        FILE *f;

        now = time(NULL);
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S ", localtime(&now));

        f = fopen(STR(DEBUG_FILENAME), "a");

        fputs(timestamp, f);

        va_start(ap, msg);
        vfprintf(f, msg, ap);
        va_end(ap);

        fputc('\n', f);
        fclose(f);
    }
#else
    #define debug_log
#endif


void return_error(char *status_msg, char *msg, ...) {
    va_list ap;
    char *formatted_msg;

    va_start(ap, msg);
    vasprintf(&formatted_msg, msg, ap);
    va_end(ap);

    fprintf(stdout, "Status: %s\r\nContent-Type: text/plain\r\n\r\n%s\r\n",
        status_msg, formatted_msg);
    exit(1);
}


int read_char() {
    int ch;

    if (read(0, &ch, 1) != 1)
        return_error("500 Internal Error", "SCGI stream truncated");
    return ch;
}


char * read_scgi_environment() {
    char *headers;
    char *name;
    char *value;
    int length;
    char ch;

    ch = read_char();
    if (isdigit(ch)) {
        length = ch - '0';
    } else {
        return_error("500 Internal Error", "SCGI stream didn't start with a digit, started with char 0x%x", ch);
    }

    while (1) {
        ch = read_char();
        if (isdigit(ch)) {
            length = (length * 10) + (ch - '0');
            if (length < 0 || length > MAX_HEADER_LENGTH) {
                return_error("500 Internal Error", "SCGI Header length is not in the range 0..%d", MAX_HEADER_LENGTH);
            }
        } else {
            if (ch == ':') {
                break;
            } else {
               return_error("500 Invalid SCGI header", "Invalid character 0x%x in length", ch);
            }
        }
    }

    debug_log("SCGI header length = %d", length);

    headers = (char *) malloc(length+1);  // +1 is for the comma after the headers
    if (!headers) {
        return_error("500 Internal Error", "Can't allocate memory to read SCGI header");
    }

    if (read(0, headers, length+1) != length+1) {
        return_error("500 Internal Error", "SCGI Header truncated");
    }

    if (headers[length] != ',') {
        return_error("500 Internal Error", "SCGI Header: Incomplete netstring, missing comma");
    }

    if (length) {
        name = headers;
        while (name < (headers + length)) {
            value = name + strlen(name) + 1;
            if (value >= (headers + length))
                return_error("500 Internal Error", "SCGI Header: Corrupt name/value table");
            setenv(name, value, 1);
            debug_log("Set [%s]=[%s]", name, value);
            name = value + strlen(value) + 1;
        }
    }
}

int main(int argc, char **argv) {
    int i;
    char *script_filename;
    char *check_directory = NULL;
    char *new_argv[2];
    char **exec_args = new_argv;

    debug_log("-------- Starting, argc=%d", argc);
    for (i = 0; i < argc; i++) {
        debug_log("argv[%d] == [%s]", i, argv[i]);
    }

    read_scgi_environment();

    /* Make things look like a CGI environment */
    unsetenv("SCGI");
    setenv("GATEWAY_INTERFACE", "CGI/1.1", 1);

    script_filename = getenv("SCRIPT_FILENAME");
    new_argv[0] = script_filename;
    new_argv[1] = NULL;

    if (argc > 1) {
        if (argv[1][strlen(argv[1]) - 1] == '/') {
            check_directory = argv[1];
        } else {
            /* specific script and maybe args in inetd config */
            script_filename = argv[1];
            exec_args = &argv[1];
        }
    }

    if (!script_filename) {
        return_error("500 Internal Error", "CGI environment missing SCRIPT_FILENAME");
    }

    if (strstr(script_filename, "../")) {
        return_error("500 Internal Error", "SCRIPT_FILENAME should not include \"../\"");
    }

    if (check_directory) {
        if (strncmp(check_directory, script_filename, strlen(check_directory))) {
            return_error("500 Internal Error",
                "[%s] doesn't reside under [%s]",
                script_filename, check_directory);
        }
    }

    execve(script_filename, exec_args, environ);

    /* If execve returned, something went wrong */
    if (errno == ENOENT) {
        return_error("404 Not Found", "Can't locate CGI script\n");
    }

    return_error("500 Internal Error",
        "Unable to execute CGI script, please contact the system administrator\n%s\n", strerror(errno));
}
