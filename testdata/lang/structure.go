package structure

func run(value int) int {
	adjust := func(step int) int {
		for _, item := range []int{1, 2} {
			if item > 0 {
				switch item {
				case 1:
					value += step
				default:
					value++
				}
			}
		}
		return value
	}
	select {
	case <-make(chan int):
		return 0
	default:
		return adjust(1)
	}
}
