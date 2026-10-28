# Makefile to compile the IEEE Conference Technical Report
TARGET = main

all: $(TARGET).pdf

$(TARGET).pdf: $(TARGET).tex references.bib IEEEtran.cls figures/fig2_app_flowchart.pdf figures/fig3_state_machine.pdf
	pdflatex $(TARGET)
	bibtex $(TARGET)
	pdflatex $(TARGET)
	pdflatex $(TARGET)

clean:
	rm -f *.aux *.bbl *.blg *.log *.out $(TARGET).pdf

.PHONY: all clean
