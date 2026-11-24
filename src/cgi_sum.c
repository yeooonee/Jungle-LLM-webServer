#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

static bool parse_query(const char *query, intmax_t values[2])
{
    if (query == NULL) {
        return false;
    }
    bool seen[2] = {false, false};
    while (*query != '\0') {
        char key = *query++;
        if ((key != 'a' && key != 'b') || *query != '=') {
            return false;
        }
        size_t index = (size_t)(key - 'a');
        if (seen[index]) {
            return false;
        }
        query++;
        if (*query != '-' && (*query < '0' || *query > '9')) {
            return false;
        }
        errno = 0;
        char *end;
        values[index] = strtoimax(query, &end, 10);
        if (end == query || errno == ERANGE) {
            return false;
        }
        seen[index] = true;
        if (*end == '\0') {
            break;
        }
        if (*end != '&' || end[1] == '\0') {
            return false;
        }
        query = end + 1;
    }
    return seen[0] && seen[1];
}

int main(void)
{
    intmax_t values[2];
    int result;
    if (!parse_query(getenv("QUERY_STRING"), values) ||
        (values[1] > 0 && values[0] > INTMAX_MAX - values[1]) ||
        (values[1] < 0 && values[0] < INTMAX_MIN - values[1])) {
        result = printf("Status: 400 Bad Request\r\nContent-Type: application/json\r\n\r\n"
                        "{\"error\":\"Expected a and b with a representable integer sum\"}\n");
    } else {
        result = printf("Content-Type: application/json\r\n\r\n{\"sum\":%" PRIdMAX "}\n",
                        values[0] + values[1]);
    }
    return result >= 0 && fflush(stdout) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
