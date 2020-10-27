import gc  # TODO need because ImportModule doesn't really import it?
import prac

prac.enable()


def f(x: int, s: str, k: list):
    pass


f(1, "a", [])

try:
    f("a", "a", [])
except TypeError as e:
    print(e)


try:
    f(1, 1, [])
except TypeError as e:
    print(e)


try:
    f(1, "a", ())
except TypeError as e:
    print(e)


print("done!")
