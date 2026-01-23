import sys


# ANSI escape codes for colors
class Style:
  GREEN = "\033[92m"
  RED = "\033[91m"
  YELLOW = "\033[93m"
  RESET = "\033[0m"
  BOLD = "\033[1m"


def color_print(text, color, end="\n"):
  if sys.stdout.isatty():
    print(f"{color}{text}{Style.RESET}", end=end)
  else:
    print(text, end=end)


def green(text):
  return f"{Style.GREEN}{text}{Style.RESET}" if sys.stdout.isatty() else text


def red(text):
  return f"{Style.RED}{text}{Style.RESET}" if sys.stdout.isatty() else text


def yellow(text):
  return f"{Style.YELLOW}{text}{Style.RESET}" if sys.stdout.isatty() else text


def bold(text):
  return f"{Style.BOLD}{text}{Style.RESET}" if sys.stdout.isatty() else text
