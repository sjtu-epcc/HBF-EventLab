"""CPU tests of the independent M6 page and transaction expectations."""
import sys
import unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from run_llama_integrated import page_plan, transactions


class M6ValidationContract(unittest.TestCase):
    def test_complete_rows(self):
        self.assertEqual(page_plan(75,{'row_bytes':2048,'block_tokens':2}),(38,0))

    def test_row_crosses_page_and_block(self):
        # 10 full rows, row 10 split across two pages, 5 full rows, then next block.
        self.assertEqual(page_plan(17,{'row_bytes':384,'block_tokens':16}),(4,1))
        self.assertEqual(page_plan(32,{'row_bytes':384,'block_tokens':16}),(6,2))

    def test_rows_wider_than_page(self):
        self.assertEqual(page_plan(7,{'row_bytes':6400,'block_tokens':3}),(7,7))

    def test_multiple_ubatches_publish_in_order(self):
        self.assertEqual(transactions(131,3,64),[(0,0,64),(0,64,64),(0,128,3),(1,131,1),(2,132,1)])

    def test_exact_ubatch_boundary(self):
        self.assertEqual(transactions(128,2,64),[(0,0,64),(0,64,64),(1,128,1)])


if __name__=='__main__':unittest.main()
