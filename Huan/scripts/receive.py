import sys

# 强制标准输入和输出使用 UTF-8 编码
sys.stdin.reconfigure(encoding='utf-8')
sys.stdout.reconfigure(encoding='utf-8')

text = sys.stdin.read()
print("我收到了：" + text)