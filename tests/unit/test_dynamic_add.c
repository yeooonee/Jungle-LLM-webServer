#include "dynamic/add.h"

#include <assert.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

int main(void) {
  long sum;
  assert(dynamic_add(2, 3, &sum) == 1 && sum == 5);
  assert(dynamic_add(-2, -3, &sum) == 1 && sum == -5);
  assert(dynamic_add(-2, 3, &sum) == 1 && sum == 1);
  assert(dynamic_add(0, 0, &sum) == 1 && sum == 0);
  assert(dynamic_add(LONG_MAX, 0, &sum) == 1 && sum == LONG_MAX);
  assert(dynamic_add(LONG_MIN, 0, &sum) == 1 && sum == LONG_MIN);
  assert(dynamic_add(LONG_MAX, LONG_MIN, &sum) == 1 && sum == -1);

  sum = 123;
  assert(dynamic_add(LONG_MAX, 1, &sum) == 0 && sum == 123);
  assert(dynamic_add(LONG_MIN, -1, &sum) == 0 && sum == 123);
  assert(dynamic_add(1, LONG_MAX, &sum) == 0 && sum == 123);
  assert(dynamic_add(-1, LONG_MIN, &sum) == 0 && sum == 123);
  assert(dynamic_add(2, 3, NULL) == 0);
  long left;
  long right;
  const char input[] = {'a', '=', '2', '&', 'b', '=', '3'};
  assert(dynamic_add_parse(input, sizeof(input), &left, &right) == 1);
  assert(left == 2 && right == 3);
  assert(dynamic_add(left, right, &sum) == 1 && sum == 5);
  const char *negative = "a=-12&b=003";
  assert(dynamic_add_parse(negative, strlen(negative), &left, &right) == 1);
  assert(left == -12 && right == 3);

  char boundary[128];
  int length = snprintf(boundary, sizeof(boundary), "a=%ld&b=%ld", LONG_MIN, LONG_MAX);
  assert(length > 0 && (size_t)length < sizeof(boundary));
  assert(dynamic_add_parse(boundary, (size_t)length, &left, &right) == 1);
  assert(left == LONG_MIN && right == LONG_MAX);

  const char *invalid[] = {
      "", "a=", "a=2", "a=2&", "a=&b=3", "a=2&b=", "a=-&b=3",
      "a=2&b=-", "a=2x&b=3", "a=2&b=3x", "a=+2&b=3", "a= 2&b=3",
      "a=2&b=3\n", "b=3&a=2", "a=2&b=3&c=4", "a=2&a=3", "a=%32&b=3",
      "a=9999999999999999999999999&b=3", "a=2&b=-9999999999999999999999999",
  };
  for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
    left = 42;
    right = 43;
    assert(dynamic_add_parse(invalid[i], strlen(invalid[i]), &left, &right) == 0);
    assert(left == 42 && right == 43);
  }
  const char nul_input[] = "a=2&b=3\0extra";
  assert(dynamic_add_parse(nul_input, sizeof(nul_input) - 1, &left, &right) == 0);
  assert(dynamic_add_parse(NULL, 7, &left, &right) == 0);
  assert(dynamic_add_parse(input, sizeof(input), NULL, &right) == 0);
  assert(dynamic_add_parse(input, sizeof(input), &left, NULL) == 0);
  for (size_t i = 0; i < sizeof(input); ++i) {
    assert(dynamic_add_parse(input, i, &left, &right) == 0);
  }
  puts("Dynamic addition checks passed");
  return 0;
}
