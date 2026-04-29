import argparse
import subprocess
import sys

from pathlib import Path

ROUND = 2

START = r"""
\PassOptionsToPackage{dvipsnames}{xcolor}
\documentclass[11pt,letterpaper]{article}
\usepackage[margin=1in]{geometry}
\usepackage{setspace}

\usepackage{tikz}
\usetikzlibrary{arrows}
\usepackage{filecontents}

\usepackage{mathtools}

\usepackage{subfigure}
\usepackage{float}
\usepackage{caption}
\usepackage{graphicx}
\usepackage{amsmath,amsthm,amssymb,amsfonts}
\usepackage{algorithm}
\usepackage{listings}
\usepackage[colorlinks,linkcolor=blue]{hyperref}
\usepackage{url}

\usepackage{colortbl}

\usepackage{pdfpages}

\title{Redis Trees}

\begin{document}

\maketitle

Each cell is \textbf{total thoughput}. Each benchmark was launched for $20$ seconds.

~\\

SABT and SABPT has $B = 16$.

"""

END = "\\end{document}\n"

def format_main_ops(ops):
    sops = f"%.{ROUND}g" % ops
    pos = sops.find("e+0")
    if pos == -1:
        raise RuntimeError("can not find e+0 in " + sops)
    return sops[:pos] + " \cdot {10}^{" + sops[pos+3:pos+4] + "}"

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("-etb", "--exp-table-builder", required=True, help="exp_table_builder.py frozen args")
    parser.add_argument("-k", "--key", type=int, nargs="+", required=True)
    parser.add_argument("-ops", "--operations", type=str, nargs="+", required=True)
    parser.add_argument("-o", "--output", type=Path, default="all_exp_table_builder.txt")
    
    args = parser.parse_args()

    with open(args.output, 'w') as f:
        f.write(START)

        for key in args.key:
            f.write("\section{key set size = $" + format_main_ops(key) + "$}")
            f.write("\n\n")

            for ops in args.operations:
                ip, dp, rp = list(map(float, ops.split("_")))
                fp = 1 - (ip + dp + rp)
                
                f.write("\subsection{" f"ins={ip} del={dp} rq={rp} count={fp}" + "}")
                f.write("\n\n")
                
                run_command = f"python3 exp_table_builder.py {args.exp_table_builder} -k {key} -ops {ops}"
                
                try:
                    cp = subprocess.run(run_command.split(), check=True, capture_output=True, text=True)
                    f.write(cp.stdout)
                    f.write("\n")
                except subprocess.CalledProcessError as exc:
                    print("lol")
                    print(exc)
                    sys.exit(1)
        
        f.write(END)
