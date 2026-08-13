#!/usr/bin/env python3
import argparse
import polars as pl

def fetch_trace_segment(date: str, start_row: int, row_count: int, output_path: str):
    """
    从 S3 读取指定日期的以太坊交易 trace 数据，截取指定行范围，保存为 CSV。
    """
    # 构造 S3 路径（假设每天只有一个 parquet 文件，使用通配符匹配）
    # 例如：s3://aws-public-blockchain/v1.0/eth/traces/date=2024-01-01/part-*.snappy.parquet
    s3_pattern = f"s3://aws-public-blockchain/v1.0/eth/traces/date={date}/part-*.snappy.parquet"
    
    storage_options = {
        "aws_allow_anonymous": "true",
        "skip_signature": "true",
    }
    
    print(f"[GET_TRACE] 正在读取 {date} 的数据，行范围 [{start_row}, {start_row + row_count - 1}]")
    try:
        # 使用惰性扫描，只选择需要的列，并跳过 / 限制行数
        df = (
            pl.scan_parquet(s3_pattern, storage_options=storage_options)
            .select(["block_number", "from_address", "to_address"])
            .slice(start_row, row_count)
            .collect()
        )
        print(f"✅ 成功读取 {df.height} 行")
        print(f"   列名: {df.columns}")
        print(f"   前两行示例:\n{df.head(2)}")
        
        # 写入 CSV
        df.write_csv(output_path)
        print(f"✅ 已保存到 {output_path}")
        
    except Exception as e:
        print(f"❌ 读取失败: {e}")
        print("\n可能的原因:")
        print("1. 指定的日期没有对应的 parquet 文件")
        print("2. 网络问题或 S3 访问权限不足")
        print("3. 行范围超出文件实际行数（文件总行数可能小于 start_row+row_count）")
        raise

def main():
    parser = argparse.ArgumentParser(description="从 AWS 公共区块链数据集提取以太坊 trace 片段")
    parser.add_argument("--date", required=True, help="日期，格式 YYYY-MM-DD，例如 2024-01-01")
    parser.add_argument("--start-row", type=int, required=True, help="起始行号（0-based）")
    parser.add_argument("--row-count", type=int, required=True, help="要读取的行数")
    parser.add_argument("--output", required=True, help="输出 CSV 文件路径")
    args = parser.parse_args()
    
    fetch_trace_segment(args.date, args.start_row, args.row_count, args.output)

if __name__ == "__main__":
    main()