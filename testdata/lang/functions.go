package functions

func top(value int) int {
	hidden := func() int { return 0 }
	return value + hidden()
}

type Widget struct{}

func (Widget) Method() int { return 1 }
