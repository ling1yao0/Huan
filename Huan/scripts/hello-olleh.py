import sys

# 读取 stdin 的内容
text = sys.stdin.read()

# 简单的处理：把文字翻转过来
# 比如输入 "hello" -> 输出 "olleh"
result = text[::-1]

# 打印到 stdout，让 C++ 程序读回去
print(result)